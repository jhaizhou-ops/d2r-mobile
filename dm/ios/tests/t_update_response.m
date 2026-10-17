// 真实更新响应策略回归：使用自造HTTP响应，覆盖服务故障、非法清单与远程绘制开关。
// Real update-response policy regression: authored HTTP responses cover service failures, invalid manifests and the remote renderer switch.
#import "../Sources/dm_update_response.h"
static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static void run(int status, NSString *body, int kind, int valid, int safe) {
    NSData *d = [body dataUsingEncoding:NSUTF8StringEncoding];
    NSHTTPURLResponse *r = [[NSHTTPURLResponse alloc] initWithURL:[NSURL URLWithString:@"https://updates.example.invalid/latest.json"] statusCode:status HTTPVersion:@"HTTP/1.1" headerFields:@{}];
    int b=0,u=0,s=-1,k=-1; NSString *n=nil,*i=nil;
    NSString *why=dm_update_response(d,r,nil,&b,&n,&u,&i,&s,&k);
    CHECK((why==nil)==valid); CHECK(k==kind); CHECK(s==safe);
    if(valid) CHECK(b==123 && [i isEqualToString:@"https://updates.example.invalid/install"]);
}
int main(void) { @autoreleasepool {
    NSString *good=@"{\"build\":123,\"urgent\":false,\"note\":\"test\",\"install_url\":\"https://updates.example.invalid/install\",\"render_safe\":true}";
    run(530,@"error code: 1033",3,0,0);
    run(500,good,3,0,0);
    run(503,@"{\"render_safe\":true}",3,0,0);
    run(200,@"{\"render_safe\":true}",2,0,0);
    run(404,good,2,0,0);
    run(200,good,2,1,1);
    run(200,@"not json",2,0,0);
    run(304,good,2,0,0); run(429,good,2,0,0);
    run(200,[good stringByReplacingOccurrencesOfString:@"true" withString:@"false"],2,1,0);
    NSString *large=[good stringByPaddingToLength:5000 withString:@" " startingAtIndex:0]; run(200,large,2,0,0);
    int b=0,u=0,s=1,k=0; NSString *n=nil,*i=nil;
    NSError *e=[NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorNotConnectedToInternet userInfo:nil];
    NSString *why=dm_update_response([good dataUsingEncoding:NSUTF8StringEncoding],nil,e,&b,&n,&u,&i,&s,&k);
    CHECK(why!=nil && s==0 && k==2 && b==0 && i==nil);
    printf("t_update_response: %d checks, %d failures\n",checks,fails); return fails!=0;
} }
