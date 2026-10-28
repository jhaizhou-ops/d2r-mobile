// 直接调用生产的镜头矩阵运算（xbg_cam.h）：签名、人物落地点、透视矩阵与逆、反向 Z 深度与还原、俯仰限制、跟随的阻尼与重置、第一人称、场景常量；
// 用一份按原版布局合成的主相机块（等距朝向、正交反向 Z、人物在已知位置），不需要 Vulkan 或 GPU。
// 编译运行：g++ -std=c++17 -Wall -Wextra dm/gfx/vulkan/engine/tests/cam_test.cpp -o /tmp/cam_test && /tmp/cam_test
// Call the production camera matrix math (xbg_cam.h) directly: signature, the character's ground point, perspective matrices and inverses, reversed-Z
// depth and its reconstruction, the pitch limit, follow damping and resets, first person, scene constants; with a main camera block synthesised in the
// original layout (isometric orientation, orthographic reversed Z, character at a known position), no Vulkan or GPU.
// Build and run: g++ -std=c++17 -Wall -Wextra dm/gfx/vulkan/engine/tests/cam_test.cpp -o /tmp/cam_test && /tmp/cam_test
#include "../xbg_cam.h"
#include <stdio.h>

static int failures;
#define CHECK(expression) do { if(!(expression)){printf("FAIL line %d: %s\n",__LINE__,#expression);++failures;} } while(0)
static bool near(double a,double b,double eps){return fabs(a-b)<=eps;}
static void put(uint8_t *block,size_t at,const float *m,size_t n){memcpy(block+at,m,n*sizeof(float));}
static void get(const uint8_t *block,size_t at,float *m,size_t n){memcpy(m,block+at,n*sizeof(float));}
// 行向量：o = [x y z w] × M
// Row vectors: o = [x y z w] * M
static void xf(const float *M,const float v[4],float o[4]){for(int j=0;j<4;j++)o[j]=v[0]*M[j]+v[1]*M[4+j]+v[2]*M[8+j]+v[3]*M[12+j];}
static bool identity(const float *a,const float *b,double eps){float m[16];xbg_cam_m4_mul(a,b,m);for(int i=0;i<16;i++)if(!near(m[i],i%5==0?1:0,eps))return false;return true;}

// 原版主相机块：视图（右、上、后三列），镜头在人物 (px, py, pz) 沿「后」方向 884.03 处（原版的固定距离），正交投影 40 × 22.5（16:9）、反向 Z
// The original main camera block: view (right, up, back columns), camera 884.03 along "back" from the character (px, py, pz) (the original's fixed
// distance), orthographic 40 x 22.5 (16:9), reversed Z
static void original(uint8_t *block,float px,float pz,float py=0){
    memset(block,0,XBG_CAM_BLOCK);
    const float r[3]={0.70711f,0,-0.70711f},u[3]={-0.35355f,0.86603f,-0.35355f},b[3]={0.61237f,0.5f,0.61237f};
    const float eye[3]={px+b[0]*884.03f,py+b[1]*884.03f,pz+b[2]*884.03f};
    float V[16]={r[0],u[0],b[0],0, r[1],u[1],b[1],0, r[2],u[2],b[2],0,
        -(eye[0]*r[0]+eye[1]*r[1]+eye[2]*r[2]),-(eye[0]*u[0]+eye[1]*u[1]+eye[2]*u[2]),-(eye[0]*b[0]+eye[1]*b[1]+eye[2]*b[2]),1};
    float iV[16];xbg_cam_m4_inv(V,iV);
    float P[16]={2/40.0f,0,0,0, 0,2/22.5f,0,0, 0,0,1/1000.0f,0, 0,0,1.55f,1},VP[16],iVP[16];
    xbg_cam_m4_mul(V,P,VP);xbg_cam_m4_inv(VP,iVP);
    put(block,80,V,16);put(block,144,iV,16);put(block,208,P,16);put(block,336,VP,16);put(block,400,iVP,16);
    const float depth[4]={1000,100,0,1};put(block,592,depth,4);
}
static double heading_deg(const xbg_cam_state &s){return atan2(s.fz,s.fx)*180/3.14159265358979;}
// 画面上沿正中那条视线：在水平线下几度；落到地面 y = 0 处离 (px, pz) 多远（水平距离）
// The sight line through the middle of the picture's top edge: how many degrees below the horizon; how far from (px, pz) it meets the ground y = 0 (horizontally)
static void top_ray(const float *ivp,double o[3],double d[3]){const float t1[4]={0,1,1,1},t0[4]={0,1,0.001f,1};float a[4],b[4];xf(ivp,t1,a);xf(ivp,t0,b);
    for(int i=0;i<3;i++){o[i]=a[i]/a[3];d[i]=b[i]/b[3]-o[i];}}
static double top_edge(const float *ivp){double o[3],d[3];top_ray(ivp,o,d);return atan2(-d[1],hypot(d[0],d[2]))*180/3.14159265358979;}
static double top_ground(const float *ivp,double px,double pz){double o[3],d[3];top_ray(ivp,o,d);double t=-o[1]/d[1];return hypot(o[0]+t*d[0]-px,o[2]+t*d[2]-pz);}

int main(){
    uint8_t src[XBG_CAM_BLOCK],dst[XBG_CAM_BLOCK];original(src,100,50);

    // 签名：要有签名那几行（272 字节）且整块够 608；朝向或投影不对就不认
    // Signature: needs the signature rows (272 bytes) and a whole block of 608; a wrong orientation or projection is not recognised
    CHECK(xbg_cam_sig(src,XBG_CAM_BLOCK,XBG_CAM_BLOCK));
    CHECK(xbg_cam_sig(src,272,XBG_CAM_BLOCK));
    CHECK(!xbg_cam_sig(src,271,XBG_CAM_BLOCK));
    CHECK(!xbg_cam_sig(src,XBG_CAM_BLOCK,XBG_CAM_BLOCK-1));
    {uint8_t bad[XBG_CAM_BLOCK];memcpy(bad,src,sizeof bad);float x=0.69f;memcpy(bad+80,&x,4);CHECK(!xbg_cam_sig(bad,XBG_CAM_BLOCK,XBG_CAM_BLOCK));}
    {uint8_t persp[XBG_CAM_BLOCK];memcpy(persp,src,sizeof persp);float m1=-1;memcpy(persp+208+44,&m1,4);CHECK(!xbg_cam_sig(persp,XBG_CAM_BLOCK,XBG_CAM_BLOCK));}

    // 人物位置 = 原版镜头沿视线往前固定距离；人物在低处（y = -12）时高度跟着降、水平位置不偏
    // The character's position = the fixed distance ahead of the original camera along the sight line; with the character lower (y = -12) the height
    // follows and the horizontal position does not drift
    {float px,py,pz;xbg_cam_ground(src,&px,&py,&pz);CHECK(near(px,100,1e-2) && near(py,0,1e-2) && near(pz,50,1e-2));
     uint8_t low[XBG_CAM_BLOCK];original(low,100,50,-12);xbg_cam_ground(low,&px,&py,&pz);CHECK(near(px,100,1e-2) && near(py,-12,1e-2) && near(pz,50,1e-2));}

    // 首帧：朝向重置为原版视线的水平方向；同一帧再调不再更新
    // First frame: the heading resets to the horizontal direction of the original sight line; another call in the same frame does not update
    xbg_cam_state st;xbg_cam_init(&st);xbg_cam_params par=xbg_cam_default_params(1);
    CHECK(xbg_cam_follow(&st,100,50,1,10.0,0,0,0,par.tau)==1);
    CHECK(st.have && near(st.fx,-0.70711,1e-4) && near(st.fz,-0.70711,1e-4));
    CHECK(xbg_cam_follow(&st,100,50,1,10.1,1,1,0,par.tau)==0);

    // 越肩：矩阵与逆互为逆；镜头在人物身后 dist、至少 h 高；人物在画面水平中线上、中心以下；反向 Z：近 → 1、远 → 0
    // Over-the-shoulder: the matrices and inverses invert each other; the camera is dist behind the character and at least h high; the character sits on the
    // picture's vertical centre line, below the centre; reversed Z: near -> 1, far -> 0
    memcpy(dst,src,sizeof dst);float vp[16],ivp[16];xbg_cam_build(&st,src,dst,1,&par,vp,ivp);
    {float V[16],iV[16],P[16],iP[16],VP[16],iVP[16];get(dst,80,V,16);get(dst,144,iV,16);get(dst,208,P,16);get(dst,272,iP,16);get(dst,336,VP,16);get(dst,400,iVP,16);
     CHECK(identity(V,iV,1e-4) && identity(P,iP,1e-4) && identity(VP,iVP,1e-3));
     CHECK(memcmp(VP,vp,64)==0 && memcmp(iVP,ivp,64)==0);
     CHECK(near(st.eye[0],100+0.70711*20,2e-2) && near(st.eye[2],50+0.70711*20,2e-2) && st.eye[1]>=13);
     const float feet[4]={100,0,50,1};float c[4];xf(VP,feet,c);CHECK(c[3]>0 && near(c[0]/c[3],0,1e-4) && c[1]/c[3]<0 && c[1]/c[3]>-1);
     const float n0[4]={0,0,-par.near_,1},f0[4]={0,0,-3000,1};float cn[4],cf[4];xf(P,n0,cn);xf(P,f0,cf);CHECK(near(cn[2]/cn[3],1,1e-5) && near(cf[2]/cf[3],0,1e-5));
     // 俯仰限制：画面上沿那条射线至少在水平线下 3°
     // Pitch limit: the ray through the top edge of the picture is at least 3 degrees below the horizon
     const float t1[4]={0,1,1,1},t0[4]={0,1,0.001f,1};float a[4],b[4];xf(iVP,t1,a);xf(iVP,t0,b);
     double dx=b[0]/b[3]-a[0]/a[3],dy=b[1]/b[3]-a[1]/a[3],dz=b[2]/b[3]-a[2]/a[3];CHECK(atan2(-dy,hypot(dx,dz))*180/3.14159265358979>=3-0.05);}
    // +464：首帧填本帧的视图投影；+592：(B, -A) 与正交标志 0，深度还原 距离 = B / (d + A)
    // +464: the first frame gets this frame's view-projection; +592: (B, -A) and orthographic flag 0, depth reconstruction distance = B / (d + A)
    {float prev[16];get(dst,464,prev,16);CHECK(memcmp(prev,vp,64)==0);
     float xy[2];int32_t flag;get(dst,592,xy,2);memcpy(&flag,dst+604,4);CHECK(flag==0);
     float P[16];get(dst,208,P,16);const float v100[4]={0,0,-100,1};float c[4];xf(P,v100,c);double d=c[2]/c[3];CHECK(near(xy[0]/(d-xy[1]),100,1e-2));}
    // 下一帧：+464 是上一帧改过的那份
    // Next frame: +464 holds the previous frame's modified one
    {float last[16];memcpy(last,vp,64);original(src,101,50);CHECK(xbg_cam_follow(&st,101,50,2,10.0+1/60.0,0,0,0,par.tau)==1);
     memcpy(dst,src,sizeof dst);xbg_cam_build(&st,src,dst,1,&par,vp,ivp);float prev[16];get(dst,464,prev,16);CHECK(memcmp(prev,last,64)==0);}

    // 跟随：没有宿主朝向时跟人物位移方向；一阶阻尼 dt = tau 转过 1 − 1/e；走最短的那一边；一帧位移 ≥ 20 重置
    // Follow: without a host heading, follow the displacement; first-order lag, dt = tau turns 1 - 1/e of the gap; the shorter way round; a displacement >= 20 in one frame resets
    {xbg_cam_state s;xbg_cam_init(&s);xbg_cam_follow(&s,0,0,1,1.0,0,0,0,0.2f);s.fx=1;s.fz=0;
     xbg_cam_follow(&s,0,0,2,1.2,1,0,1,0.2f);CHECK(near(heading_deg(s),90*(1-exp(-1.0)),0.2));
     s.fx=float(cos(170*3.14159265358979/180));s.fz=float(sin(170*3.14159265358979/180));
     xbg_cam_follow(&s,0,0,3,1.4,1,float(cos(-170*3.14159265358979/180)),float(sin(-170*3.14159265358979/180)),0.2f);
     double h=heading_deg(s);if(h<0)h+=360;CHECK(near(h,170+20*(1-exp(-1.0)),0.2));
     s.fx=1;s.fz=0;xbg_cam_follow(&s,0,5,4,1.6,0,0,0,0.2f);CHECK(heading_deg(s)>0 && heading_deg(s)<90);
     xbg_cam_follow(&s,0,30,5,1.8,1,1,0,0.2f);CHECK(near(s.fx,-0.70711,1e-4) && near(s.fz,-0.70711,1e-4));}

    // 第一人称：眼在人物前方 fwd、高 eye，视线下俯 pitch
    // First person: the eye is fwd ahead of the character at height eye, looking down by pitch
    {xbg_cam_state s;xbg_cam_init(&s);xbg_cam_params p2=xbg_cam_default_params(2);original(src,100,50);xbg_cam_follow(&s,100,50,1,1.0,0,0,0,p2.tau);
     memcpy(dst,src,sizeof dst);xbg_cam_build(&s,src,dst,2,&p2,vp,ivp);
     CHECK(near(s.eye[0],100-0.70711*2.2,2e-2) && near(s.eye[1],6,1e-2) && near(s.eye[2],50-0.70711*2.2,2e-2));
     CHECK(near(asin(-s.fwd[1])*180/3.14159265358979,8,1e-3) && p2.fov==75);
     // 人物在低处 12：眼睛跟着低 12（以前停在 y = 6，人像浮在空中）；越肩档镜头同样整体低 12
     // The character 12 lower: the eye is 12 lower too (it used to stay at y = 6, as if floating); the over-the-shoulder camera is 12 lower as a whole too
     uint8_t low[XBG_CAM_BLOCK],d2[XBG_CAM_BLOCK];original(low,100,50,-12);xbg_cam_state s2=s;memcpy(d2,low,sizeof d2);xbg_cam_build(&s2,low,d2,2,&p2,vp,ivp);
     CHECK(near(s2.eye[1],-6,1e-2) && near(s2.eye[0],s.eye[0],2e-2) && near(s2.eye[2],s.eye[2],2e-2) && near(s2.fwd[1],s.fwd[1],1e-5));
     xbg_cam_params p1=xbg_cam_default_params(1);xbg_cam_state a=s,b=s;a.dcur=b.dcur=0;
     memcpy(d2,src,sizeof d2);xbg_cam_build(&a,src,d2,1,&p1,vp,ivp);memcpy(d2,low,sizeof d2);xbg_cam_build(&b,low,d2,1,&p1,vp,ivp);
     CHECK(near(b.eye[1],a.eye[1]-12,1e-2) && near(b.eye[0],a.eye[0],2e-2) && near(b.fwd[1],a.fwd[1],1e-5));}

    // 片元槽 10 的场景常量：认签名，写镜头位置、视线与线性深度起点；镜头还没建过就不动
    // Scene constants of fragment slot 10: the signature is recognised; camera position, sight line and linear depth origin are written; untouched before any camera was built
    {uint8_t scene[XBG_CAM_SCENE];memset(scene,0,sizeof scene);const float dir[3]={-0.61237f,-0.5f,-0.61237f},lin[3]={1,1,550};put(scene,32,dir,3);put(scene,848,lin,3);
     CHECK(xbg_cam_sig10(scene,XBG_CAM_SCENE,XBG_CAM_SCENE) && xbg_cam_sig10(scene,44,XBG_CAM_SCENE));
     CHECK(!xbg_cam_sig10(scene,43,XBG_CAM_SCENE) && !xbg_cam_sig10(scene,XBG_CAM_SCENE,XBG_CAM_SCENE-16));
     uint8_t copy[XBG_CAM_SCENE];memcpy(copy,scene,sizeof copy);xbg_cam_state empty;xbg_cam_init(&empty);xbg_cam_build10(&empty,copy);CHECK(memcmp(copy,scene,sizeof copy)==0);
     xbg_cam_build10(&st,copy);float e[3],f[3],l[3];get(copy,16,e,3);get(copy,32,f,3);get(copy,848,l,3);
     CHECK(memcmp(e,st.eye,12)==0 && memcmp(f,st.fwd,12)==0 && l[0]==1 && l[1]==1 && l[2]==0);}

    // 避墙：第一帧直接取目标；往里拉 0.08 秒走 1 − 1/e，往外放 0.4 秒走 1 − 1/e；没有墙（0）或墙比 dist 远都回到 dist；最近不小于 2
    // Wall avoidance: the first frame takes the target directly; pulling in covers 1 - 1/e in 0.08 s, letting out covers 1 - 1/e in 0.4 s; no wall (0) or a wall
    // further than dist goes back to dist; never closer than 2
    {xbg_cam_state s;xbg_cam_init(&s);
     CHECK(near(xbg_cam_pull(&s,20,8,0),8,1e-6));
     CHECK(near(xbg_cam_pull(&s,20,0,0.4),8+12*(1-exp(-1.0)),1e-3));
     float before=s.dcur;CHECK(near(xbg_cam_pull(&s,20,4,0.08),before+(4-before)*(1-exp(-1.0)),1e-3));
     xbg_cam_state f;xbg_cam_init(&f);CHECK(near(xbg_cam_pull(&f,20,30,0),20,1e-6));
     xbg_cam_state g;xbg_cam_init(&g);CHECK(near(xbg_cam_pull(&g,20,0.5f,0),2,1e-6));}
    // 拉近后：镜头在人物身后 dcur，沿「吊臂」升高（到看点高度的距离不变，再受俯仰限制），画面上沿仍在水平线下 3° 以上，上沿视线落地不超过人前方 reach
    // Pulled in: the camera is dcur behind the character and rises along the "boom" (its distance to the look height stays, then the pitch limit applies);
    // the picture's top edge is still at least 3 degrees below the horizon, and its sight line meets the ground no further than reach ahead of the character
    {xbg_cam_state s;xbg_cam_init(&s);xbg_cam_params p=xbg_cam_default_params(1);original(src,100,50);xbg_cam_follow(&s,100,50,1,1.0,0,0,0,p.tau);xbg_cam_pull(&s,p.dist,8,0);
     memcpy(dst,src,sizeof dst);xbg_cam_build(&s,src,dst,1,&p,vp,ivp);
     const double boom=p.lookh+sqrt(p.dist*p.dist+(p.h-p.lookh)*(p.h-p.lookh)-64.0);
     CHECK(near(hypot(s.eye[0]-100,s.eye[2]-50),8,2e-2) && s.eye[1]>=boom-1e-3);
     CHECK(top_edge(ivp)>=3-0.05 && top_ground(ivp,100,50)<=p.reach+0.5);
     // 人还在画面中间一带：脚在中线以下、不到下半屏的 3/4（不沉到底栏底下）
     // The character stays around the middle of the picture: the feet below the centre line but within three quarters of the lower half (not sunk under the bottom bar)
     const float feet[4]={100,0,50,1};float c[4];xf(vp,feet,c);CHECK(c[3]>0 && c[1]/c[3]<0 && c[1]/c[3]>-0.75);}
    // 不拉近时也受 reach 限制：上沿视线落地在人前方 reach 以内（原来只要求 3°，会看到游戏还没画的远处）
    // Not pulled in, reach still applies: the top-edge sight line meets the ground within reach ahead of the character (only 3 degrees used to be required,
    // which showed far ground the game had not drawn yet)
    {xbg_cam_state s;xbg_cam_init(&s);xbg_cam_params p=xbg_cam_default_params(1);original(src,100,50);xbg_cam_follow(&s,100,50,1,1.0,0,0,0,p.tau);
     memcpy(dst,src,sizeof dst);xbg_cam_build(&s,src,dst,1,&p,vp,ivp);
     CHECK(near(hypot(s.eye[0]-100,s.eye[2]-50),20,2e-2) && top_ground(ivp,100,50)<=p.reach+0.5 && top_ground(ivp,100,50)>p.reach-5);
     xbg_cam_params far=p;far.reach=0;xbg_cam_state t;xbg_cam_init(&t);xbg_cam_follow(&t,100,50,1,1.0,0,0,0,p.tau);xbg_cam_build(&t,src,dst,1,&far,vp,ivp);
     CHECK(near(top_edge(ivp),3,0.05));}

    // 体感转向：正 = 往画面右边转（朝向转向 右 = (−f.z, f.x)）；hold 时跟随不动朝向（宿主的朝向、位移都不管），但换区域照样重置
    // Sensor steering: positive = towards the right of the picture (the heading turns towards right = (-f.z, f.x)); with hold the follow leaves the
    // heading alone (neither the host's heading nor the displacement), but an area change still resets it
    {xbg_cam_state s;xbg_cam_init(&s);xbg_cam_follow(&s,0,0,1,1.0,0,0,0,0.2f);s.fx=1;s.fz=0;
     const float rx=-s.fz,rz=s.fx;xbg_cam_turn(&s,0.1f);CHECK(s.fx*rx+s.fz*rz>0 && near(heading_deg(s),0.1*180/3.14159265358979,1e-3));
     s.hold=1;const float hx=s.fx,hz=s.fz;xbg_cam_follow(&s,0,3,2,1.2,1,0,1,0.2f);CHECK(s.fx==hx && s.fz==hz);
     xbg_cam_follow(&s,0,40,3,1.4,1,0,1,0.2f);CHECK(near(s.fx,-0.70711,1e-4) && near(s.fz,-0.70711,1e-4));
     s.hold=0;xbg_cam_follow(&s,0,40,4,1.6,1,0,1,0.2f);CHECK(heading_deg(s)>-135+1);}

    // 玩家上下拖（tilt）：0 = 缺省机位不变；正 = 绕看点往平处转（水平距离不超过缺省、只降高度）；负 = 更俯视、绕看点往近处转；
    // 夹在视线下俯 4°–75°，tilt_lo / tilt_hi 给出这个范围；第一人称直接加到俯仰上。期望值按圆弧手算，不调被测函数
    // The player's vertical drag (tilt): 0 = the default rig unchanged; positive = orbit towards level (horizontal distance at most the default, only lower);
    // negative = steeper, orbiting closer; clamped to a 4-75 degree look-down, with tilt_lo / tilt_hi giving that range; first person adds it to the pitch.
    // Expected values are worked out on the arc by hand, not by calling the code under test
    {const double deg=3.14159265358979/180;xbg_cam_state t0=st;t0.tilt=0;uint8_t d0[XBG_CAM_BLOCK];memcpy(d0,src,sizeof d0);float v0[16],i0[16];xbg_cam_build(&t0,src,d0,1,&par,v0,i0);
     const double hd0=hypot(t0.eye[0]-100,t0.eye[2]-50),h0=t0.eye[1],run=hd0+par.look,rise=h0-par.lookh,R=hypot(run,rise),e0=atan2(rise,run);
     CHECK(near(hd0,20,2e-2) && near(t0.tilt_hi,e0-4*deg,1e-3) && near(t0.tilt_lo,e0-75*deg,1e-3));
     auto rig=[&](float tilt,double &hd,double &h){xbg_cam_state t=st;t.tilt=tilt;uint8_t d1[XBG_CAM_BLOCK];memcpy(d1,src,sizeof d1);float v1[16],i1[16];xbg_cam_build(&t,src,d1,1,&par,v1,i1);hd=hypot(t.eye[0]-100,t.eye[2]-50);h=t.eye[1];};
     double hd,h;rig(float(10*deg),hd,h);CHECK(near(hd,20,2e-2) && near(h,par.lookh+R*sin(e0-10*deg),2e-2) && h<h0);
     rig(1.4f,hd,h);CHECK(near(hd,20,2e-2) && near(h,par.lookh+R*sin(4*deg),2e-2));
     rig(-0.3f,hd,h);CHECK(near(hd,R*cos(e0+0.3)-par.look,2e-2) && near(h,par.lookh+R*sin(e0+0.3),2e-2) && hd<20);
     xbg_cam_params fp=xbg_cam_default_params(2);xbg_cam_state t2=st;t2.tilt=float(5*deg);uint8_t d2[XBG_CAM_BLOCK];memcpy(d2,src,sizeof d2);float v2[16],i2[16];xbg_cam_build(&t2,src,d2,2,&fp,v2,i2);
     CHECK(near(atan2(-t2.fwd[1],hypot(t2.fwd[0],t2.fwd[2])),(fp.pitch-5)*deg,1e-3) && near(t2.tilt_hi,(fp.pitch+10)*deg,1e-4));}
    if(failures){printf("%d failure(s)\n",failures);return 1;}
    printf("cam_test: all checks passed\n");return 0;
}
