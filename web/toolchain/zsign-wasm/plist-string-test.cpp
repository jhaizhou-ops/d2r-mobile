// 仅使用合成 plist 测试派生 XML 解析器，不读取证书、密钥或游戏内容。
// Test the derived XML reader using synthetic plists only, without
// certificates, keys or game data.
#include "json.h"
#include <cassert>
#include <cstdio>
#include <string>
using namespace std;

static bool parse(const string &value, jvalue &out) {
    return out.read_plist("<plist><dict><key>value</key><string>" + value +
                          "</string></dict></plist>");
}

int main() {
    jvalue value;
    assert(parse("A &amp; B &lt; &gt; &quot; &apos;", value));
    assert(value["value"].as_string() == "A & B < > \" '");
    string encoded;
    value.style_write_plist(encoded);
    assert(encoded.find("&amp;amp;") == string::npos);
    jvalue roundtrip;
    assert(roundtrip.read_plist(encoded));
    assert(roundtrip["value"].as_string() == value["value"].as_string());
    assert(parse("&amp;lt; &#38;lt; &#x26;lt;", value));
    assert(value["value"].as_string() == "&lt; &lt; &lt;");
    assert(parse("]] &amp;#65; ]]&gt; &#13;", value));
    value.style_write_plist(encoded);
    assert(encoded.find("]]>") == string::npos);
    assert(roundtrip.read_plist(encoded));
    assert(roundtrip["value"].as_string() == "]] &#65; ]]> \r");
    assert(parse("&#65;&#x20AC;&#128512;", value));
    assert(value["value"].as_string() == "A\xe2\x82\xac\xf0\x9f\x98\x80");
    assert(parse(" \tline\r\nnext\rfinal\n &#13; ", value));
    assert(value["value"].as_string() == " \tline\nnext\nfinal\n \r ");
    assert(value.read_plist("<plist><dict><key> \tA &amp; B\n "
                            "</key><string>x</string></dict></plist>"));
    assert(value[" \tA & B\n "].as_string() == "x");
    assert(
        value.read_plist("<plist><dict><key>data</key><data>Y\nQ\t==\r\n</data></dict></plist>"));
    assert(value["data"].as_data() == "a");
    for (const string &invalid :
         {"&unknown;", "&amp", "&#;", "&#x;", "&#0;", "&#xD800;", "&#x110000;",
          "&#9999999999999999999999;", "&#-1;", "&#xGG;", "&#12;", "&#xFFFE;", "&#65junk;"}) {
        assert(!parse(invalid, value));
        assert(!value.read_plist("<plist><dict><key>" + invalid + "</key><true/></dict></plist>"));
    }
    assert(!parse(string("nul\0byte", 8), value));
    puts("PASS synthetic XML entity and whitespace round-trips");
}
