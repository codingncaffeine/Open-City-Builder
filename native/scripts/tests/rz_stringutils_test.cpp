#include <cstdio>
#include <cstring>
#include "rz/RZStringUtils.h"
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
int main() {
    char buf[64];
    std::strcpy(buf, "Hello, World 9");
    CHECK(std::strcmp(RZStruprAImp(buf), "HELLO, WORLD 9") == 0);
    CHECK(std::strcmp(RZStrlwrAImp(buf), "hello, world 9") == 0);
    // the wide variants shift every value <= 0x7f by 0x20, letters or not
    CHECK(RZToupperWImp('a') == 'A' && RZToupperWImp('1') == 0x11 && RZToupperWImp(0x100) == 0x100);
    CHECK(RZTolowerWImp('A') == 'a' && RZTolowerWImp('!') == 0x41 && RZTolowerWImp(0xe9) == 0xe9);

    CHECK(RZAtofEnglishA("3.5") == 3.5 && RZAtofEnglishA("-0.25") == -0.25 && RZAtofEnglishA("  12") == 12.0);
    char* end = nullptr;
    CHECK(RZStrtodEnglishA("1.5e2xyz", &end) == 150.0 && std::strcmp(end, "xyz") == 0);
    CHECK(RZStrtodEnglishA("2e-2", &end) == 0.02);
    CHECK(RZAtoi64A("  -1234567890123") == -1234567890123LL && RZAtoi64A("+42abc") == 42 && RZAtoi64A("x") == 0);
    CHECK(RZAtoUint32HexA("0xDEADBEEF") == 0xdeadbeef && RZAtoUint32HexA("ff") == 255);
    CHECK(RZAtoUint64HexA("0x123456789abcdef0") == 0x123456789abcdef0ULL);

    RZItoaAImp(-255, buf, 10);
    CHECK(std::strcmp(buf, "-255") == 0);
    RZItoaAImp(255, buf, 16);
    CHECK(std::strcmp(buf, "ff") == 0);
    RZLtoaAImp(-5, buf, 2);          // sign dropped for non-decimal radix
    CHECK(std::strcmp(buf, "101") == 0);
    RZLtoaAImp(7, buf, 40);          // invalid radix -> empty string
    CHECK(buf[0] == '\0');

    CHECK(RZFtoaEnglishA(3.14159, buf, 32, 3) && std::strcmp(buf, "3.141") == 0);
    CHECK(RZFtoaEnglishA(0.5, buf, 32, 6) && std::strcmp(buf, "0.5") == 0);
    CHECK(RZFtoaEnglishA(-2.0, buf, 32, 6) && std::strcmp(buf, "-2") == 0);
    CHECK(RZFtoaEnglishA(100.0, buf, 32, 2) && std::strcmp(buf, "100") == 0);
    // maxFraction counts digits after the leading zeros (values exact in binary: the C library's fcvt rounds 0.001 differently per platform)
    CHECK(RZFtoaEnglishA(0.0625, buf, 32, 2) && std::strcmp(buf, "0.062") == 0);
    CHECK(RZFtoaEnglishA(0.125, buf, 32, 2) && std::strcmp(buf, "0.12") == 0);
    CHECK(!RZFtoaEnglishA(123456.0, buf, 3, 2));

    const std::uint8_t data[4] = {0x0a, 0xff, 0x10, 0x5c};
    char hex[16];
    ConvertBinaryDataToASCIIArray(data, 4, hex);
    CHECK(std::strncmp(hex, "0AFF105C", 8) == 0 && hex[8] == '0');
    std::uint8_t back[4];
    CHECK(ConvertASCIIArrayToBinaryData("0AFF105C", 8, back) && std::memcmp(back, data, 4) == 0);
    CHECK(!ConvertASCIIArrayToBinaryData("0AZZ", 4, back));

    cRZString items[6];
    unsigned count = 6;
    CHECK(RZParseDelimitedText("a, b ,\"c d\", e", 14, ',', items, count) && count == 4);
    CHECK(std::strcmp(items[0].ToChar(), "a") == 0 && std::strcmp(items[1].ToChar(), "b") == 0 &&
          std::strcmp(items[2].ToChar(), "c d") == 0 && std::strcmp(items[3].ToChar(), "e") == 0);
    count = 2;
    CHECK(!RZParseDelimitedText("1 2 3 4", 7, ' ', items, count) && count == 2);
    count = 6;
    CHECK(RZParseDelimitedText("   ", 3, ',', items, count) && count == 0);

    std::uint16_t w[7];
    memset16(w, 0xabcd, 7);
    CHECK(w[0] == 0xabcd && w[6] == 0xabcd);
    std::uint32_t d[3];
    memset32(d, 0x01020304, 3);
    CHECK(d[2] == 0x01020304);
    std::puts(failures ? "rz_stringutils: FAILED" : "rz_stringutils ok");
    return failures != 0;
}
