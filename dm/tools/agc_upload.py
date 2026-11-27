#!/usr/bin/env python3
# 把鸿蒙 .app 正式包传到 AppGallery Connect（AGC Publishing API），可选再提交审核。只用标准库。
#   python agc_upload.py --cred <凭据.json> check                      只取令牌、查应用 ID（不上传）
#   python agc_upload.py --cred <凭据.json> upload <包.app>            上传并绑定到草稿版本（不提交审核）
#   python agc_upload.py --cred <凭据.json> upload <包.app> --submit   上传、绑定，等解析后提交审核
#   python agc_upload.py --cred <凭据.json> groups                     只读：列出测试群组（外部 / 内部）和上一个邀请测试版本挂的群组
#   python agc_upload.py --cred <凭据.json> test <包.app> --desc 说明 [--submit]   邀请测试：传包、建测试版本、挂全部群组（并集），可选提交
# 凭据文件放在 Git 之外，两种都行：
#   Service Account（AGC 推荐）：创建时下载的 *private.json 原样给 --cred（要 cryptography 包）；
#   API 客户端（旧）：自己写 {"client_id": "...", "client_secret": "..."}。
# 角色用「管理员」或「APP 管理员」。可在同一文件里加 "package": "com.xiao.boluo"、"host": "connect-api.cloud.huawei.com"，或用 --package / --host。
# Upload a HarmonyOS .app release package to AppGallery Connect (Publishing API), optionally submit it for review. Standard library only.
#   check: token + app id only. upload: upload and bind to the draft version. --submit: also submit for review after parsing.
# The credential file stays outside Git: {"client_id", "client_secret"} of a team-level AGC API client (project N/A; role
# administrator or app administrator), optional "package" and "host".
# Flow (developer.huawei.com, AGC Publishing API): POST /api/oauth2/v1/token -> GET /api/publish/v2/appid-list ->
# GET /api/publish/v2/upload-url/for-obs -> PUT the file to the returned URL with the returned headers ->
# PUT /api/publish/v3/app-package-info -> (POST /api/publish/v3/app-submit).
import argparse, base64, hashlib, http.client, json, os, sys, time, urllib.error, urllib.parse, urllib.request


def log(*a):
    print(*a, flush=True)


def b64url(b):
    return base64.urlsafe_b64encode(b).rstrip(b'=').decode()


def service_jwt(sa, now=None):
    """Service Account (AGC 推荐): the JWT itself is the bearer credential. Header kid/typ/alg=PS256; payload aud = token_uri, iss = sub_account,
    iat, exp = iat + 3600; signed SHA256withRSA/PSS with the private key from private.json. Needs the cryptography package."""
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import padding
    pem = sa['private_key'].strip()
    if not pem.startswith('-----'): pem = '-----BEGIN PRIVATE KEY-----\n%s\n-----END PRIVATE KEY-----' % pem
    key = serialization.load_pem_private_key(pem.encode(), password=None)
    iat = int(now if now is not None else time.time())
    head = b64url(json.dumps({'kid': sa['key_id'], 'typ': 'JWT', 'alg': 'PS256'}, separators=(',', ':')).encode())
    body = b64url(json.dumps({'aud': sa.get('token_uri') or 'https://oauth-login.cloud.huawei.com/oauth2/v3/token', 'iss': sa['sub_account'],
                              'iat': iat, 'exp': iat + 3600}, separators=(',', ':')).encode())
    sig = key.sign(('%s.%s' % (head, body)).encode(), padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32), hashes.SHA256())
    return '%s.%s.%s' % (head, body, b64url(sig))


class Agc:
    def __init__(self, cred):
        self.host = cred.get('host', 'connect-api.cloud.huawei.com'); self.token = None; self.made = 0
        self.sa = cred if 'private_key' in cred and 'sub_account' in cred else None   # private.json of a Service Account
        if not self.sa: self.cid = cred['client_id']; self.secret = cred['client_secret']   # legacy API client

    def call(self, method, path, query=None, body=None, auth=True, headers=None):
        url = 'https://%s%s' % (self.host, path) + ('?' + urllib.parse.urlencode(query) if query else '')
        h = {'Content-Type': 'application/json'}; h.update(headers or {})
        if auth and self.sa:
            if time.time() - self.made > 3000: self.token = service_jwt(self.sa); self.made = time.time()   # valid 3600 s
            h['Authorization'] = 'Bearer ' + self.token
        elif auth: h.update({'client_id': self.cid, 'Authorization': 'Bearer ' + self.token})
        req = urllib.request.Request(url, data=json.dumps(body).encode() if body is not None else None, headers=h, method=method)
        try:
            with urllib.request.urlopen(req, timeout=120) as r: d = json.loads(r.read() or b'{}')
        except urllib.error.HTTPError as e:
            sys.exit('[x] %s %s -> HTTP %d %s' % (method, path, e.code, e.read()[:400].decode('utf-8', 'replace')))
        ret = d.get('ret')
        if ret and ret.get('code') not in (0, None):
            sys.exit('[x] %s %s -> ret %s' % (method, path, json.dumps(ret, ensure_ascii=False)))
        if d.get('rtnCode') not in (0, None, '0'):   # /api/app-test/v1/...: rtnCode instead of ret
            sys.exit('[x] %s %s -> rtnCode %s %s' % (method, path, d.get('rtnCode'), d.get('rtnDesc', '')))
        return d

    def login(self):
        if self.sa: log('[ok] Service Account %s, key %s' % (self.sa['sub_account'], self.sa['key_id'])); return
        d = self.call('POST', '/api/oauth2/v1/token', body={'grant_type': 'client_credentials', 'client_id': self.cid, 'client_secret': self.secret}, auth=False)
        self.token = d.get('access_token') or sys.exit('[x] no access_token: %s' % d)
        log('[ok] token, expires in %s s' % d.get('expires_in'))

    def app_id(self, package):
        d = self.call('GET', '/api/publish/v2/appid-list', {'packageName': package, 'packageTypes': 7})
        ids = d.get('appids') or []
        for x in ids:
            if x.get('key') == package: return x.get('value')
        # 实测（2026-11-27）：按包名查时 key 是应用名（如 "D2R Mobile"），只回一条就是它 | measured: queried by package, the key is the
        # app name (e.g. "D2R Mobile"); a single result is the app
        if len(ids) == 1 and ids[0].get('value'): log('[ok] app "%s"' % ids[0].get('key')); return ids[0]['value']
        sys.exit('[x] %s has no app id in this team: %s' % (package, d))


def put_file(info, path, size):
    u = urllib.parse.urlsplit(info['url']); headers = dict(info.get('headers') or {}); headers['Content-Length'] = str(size)
    c = (http.client.HTTPSConnection if u.scheme == 'https' else http.client.HTTPConnection)(u.netloc, timeout=600)
    t = time.time()
    with open(path, 'rb') as f:
        c.request(info.get('method', 'PUT'), u.path + ('?' + u.query if u.query else ''), body=f, headers=headers)
        r = c.getresponse(); body = r.read()
    if r.status != 200: sys.exit('[x] file upload -> HTTP %d %s' % (r.status, body[:400]))
    log('[ok] uploaded %.1f MB in %.0f s' % (size / 1048576, time.time() - t))


def upload(a, appid, path, release_type):
    """Upload-URL + PUT; returns (fileName, objectId)."""
    name = os.path.basename(path); size = os.path.getsize(path)
    if not name.endswith('.app'): sys.exit('[x] AGC 只收 .app 包 | AGC accepts .app packages only')
    if size > 4 << 30: sys.exit('[x] 超过 4 GB 上限 | over the 4 GB limit')
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''): h.update(b)
    q = {'appId': appid, 'fileName': name, 'contentLength': size, 'sha256': h.hexdigest()}
    if release_type is not None: q['releaseType'] = release_type
    d = a.call('GET', '/api/publish/v2/upload-url/for-obs', q)
    info = d.get('urlInfo') or sys.exit('[x] no urlInfo: %s' % d)
    put_file(info, path, size)
    return name, info['objectId']


# ---- 邀请测试（Testing API；字段按官方数据结构页 2026-11-27 核对，首次真跑前没验过）| Invitation test (Testing API; fields checked against the
# official data-structure pages 2026-11-27, not yet run against AGC) ----
# 测试群组属于应用、一直在；但新测试版本只发给它列出的群组。所以每次取「全部现有群组 ∪ 上一个邀请测试版本挂的群组」全部挂上：测试名单不会变少。
# Test groups belong to the app and persist, but a new test version only reaches the groups it lists: every run attaches the union of all
# existing groups and the previous invitation-test version's groups, so the tester list never shrinks.
def test_groups(a, appid):
    """GET /api/app-test/v1/test-group/list (appId in a header; external 0 and internal 1, every page)."""
    out = []
    for gtype in (0, 1):
        page = 1
        while True:
            try: d = a.call('GET', '/api/app-test/v1/test-group/list', {'groupType': gtype, 'current': page, 'pageSize': 50}, headers={'appId': str(appid)})
            except SystemExit as e:   # 实测：API 客户端角色不够时 rtnCode 2「permission denied by api permission control」| measured: rtnCode 2 when the role lacks it
                log('[!] 查不到群组列表（%s），只用上一个测试版本挂的群组 | group list unavailable, using the previous test version groups' % e); return out
            out += [g for g in d.get('groups') or [] if g.get('groupId')]
            if page >= int((d.get('pageInfo') or {}).get('totalPage') or 1): break
            page += 1
    return out


def previous_groups(a, appid):
    """Groups bound to the latest invitation-test version: POST /api/publish/v3/version/brief-info/list, then GET /api/publish/v3/app-info."""
    d = a.call('POST', '/api/publish/v3/version/brief-info/list', None, {}, headers={'appId': str(appid)})
    tests = [v for v in d.get('versionList') or [] if str(v.get('testType')) == '3' and v.get('versionId')]
    if not tests: return []
    v = tests[0]
    info = a.call('GET', '/api/publish/v3/app-info', {'appId': appid, 'releaseType': 6, 'versionId': v['versionId']})
    gi = (((info.get('openTestInfo') or {}).get('testTaskInfo') or {}).get('groupInfos')) or []
    return [g for g in gi if g.get('groupId') and str(g.get('bind', 1)) == '1']


def test_flow(a, appid, o):
    if len(o.desc) > 50: sys.exit('[x] --desc 最长 50 字 | --desc is at most 50 characters')
    groups = {g['groupId']: g for g in test_groups(a, appid)}
    for g in previous_groups(a, appid): groups.setdefault(g['groupId'], g)
    if not groups: sys.exit('[x] 没查到测试群组：不建空名单的版本 | no test groups found: refusing to create a version without testers')
    log('[ok] %d test groups: %s' % (len(groups), ', '.join('%s(%s)' % (g.get('groupName', ''), g.get('addedTestersNum', g.get('addedTesterNum', '?'))) for g in groups.values())))
    name, oid = upload(a, appid, o.app, 6)   # 6 = HarmonyOS 测试发布 | HarmonyOS test release (API >= 10)
    d = a.call('POST', '/api/publish/v2/test/version/pkg', {'appId': appid}, {'file': {'fileName': name, 'objectId': oid}})
    pkg = (d.get('pkgVersion') or [None])[0] or sys.exit('[x] no pkgVersion: %s' % d)
    log('[ok] test package %s, waiting for AGC to parse it' % pkg)
    for _ in range(60):
        time.sleep(10)
        st = (a.call('GET', '/api/publish/v3/package/compile/status', {'appId': appid, 'pkgIds': pkg}).get('pkgStateList') or [{}])[0]
        state = str(st.get('successStatus'))
        if state == '0': break                      # 0 正常 | parsed
        if state == '2': sys.exit('[x] package parse failed: %s' % st)
    else: sys.exit('[x] package still not parsed after 10 min')
    d = a.call('POST', '/api/publish/v2/test/app/version', {'appId': appid}, {'releaseType': 6, 'testType': 3, 'testDesc': o.desc})
    ver = d.get('versionId') or sys.exit('[x] no versionId: %s' % d)
    now = int(time.time() * 1000); end = now + o.days * 86400 * 1000
    body = {'versionId': ver, 'pkgId': pkg, 'openTestInfo': {'startTime': now, 'endTime': end, 'testDesc': o.desc,
            'testTaskInfo': {'groupInfos': [{'groupId': gid, 'bind': 1} for gid in groups], 'displayArea': '1', 'needNotify': 1}}}
    a.call('PUT', '/api/publish/v2/test/app/version', {'appId': appid}, body)
    log('[ok] test version %s with %d groups (until %s)' % (ver, len(groups), time.strftime('%Y-%m-%d', time.localtime(end / 1000))))
    if not o.submit: log('没有提交（加 --submit 提交，或在 AGC 网页上提交）| not submitted (add --submit or submit in the console)'); return
    a.call('POST', '/api/publish/v2/test/app/version/submit', {'appId': appid}, {'versionId': ver}); log('[ok] test version submitted')


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--cred', required=True); ap.add_argument('--package'); ap.add_argument('--host')
    sp = ap.add_subparsers(dest='cmd', required=True); sp.add_parser('check'); sp.add_parser('groups')
    u = sp.add_parser('upload'); u.add_argument('app'); u.add_argument('--submit', action='store_true'); u.add_argument('--remark', default='')
    u.add_argument('--release-type', type=int, default=1, help='1 = 全网发布 | full release')
    t = sp.add_parser('test', help='邀请测试 | invitation test'); t.add_argument('app'); t.add_argument('--desc', required=True, help='测试说明 | test notes')
    t.add_argument('--days', type=int, default=90, help='测试期（天，最长 90）| test period in days (max 90)'); t.add_argument('--submit', action='store_true')
    o = ap.parse_args()
    cred = json.load(open(o.cred, encoding='utf-8')); package = o.package or cred.get('package', 'com.xiao.boluo')
    if o.host: cred['host'] = o.host
    a = Agc(cred); a.login(); appid = a.app_id(package); log('[ok] %s app id %s' % (package, appid))
    if o.cmd == 'check': return
    if o.cmd == 'groups':
        for g in test_groups(a, appid): log('  %s  %s  type=%s testers=%s' % (g.get('groupId'), g.get('groupName', ''), g.get('groupType', ''), g.get('addedTestersNum', '?')))
        for g in previous_groups(a, appid): log('  上一个邀请测试版本挂着 | bound to the previous test version: %s %s' % (g.get('groupId'), g.get('groupName', '')))
        return
    if o.cmd == 'test':
        if not 1 <= o.days <= 90: sys.exit('[x] --days 1..90')
        test_flow(a, appid, o); return
    path = o.app; name, oid = upload(a, appid, path, None); info = {'objectId': oid}
    d = a.call('PUT', '/api/publish/v3/app-package-info', {'appId': appid, 'releaseType': o.release_type}, {'fileName': name, 'objectId': info['objectId']})
    log('[ok] bound to the draft version: %s' % json.dumps({k: v for k, v in d.items() if k != 'ret'}, ensure_ascii=False))
    if not o.submit:
        log('没有提交审核（要提交加 --submit，或在 AGC 网页上点提交）| not submitted for review (add --submit or submit in the console)'); return
    log('等 AGC 解析包（约 2 分钟）| waiting for AGC to parse the package (~2 min)'); time.sleep(150)
    body = {'releaseType': o.release_type}
    if o.remark: body['remark'] = o.remark
    a.call('POST', '/api/publish/v3/app-submit', {'appId': appid}, body); log('[ok] submitted for review')


if __name__ == '__main__':
    main()
