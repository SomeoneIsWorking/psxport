// field_owned_leaves.cpp — byte-faithful batch ownership of field-spine substrate leaves (USER max-throughput
// wave pipeline). Each = a substrate fn dispatched by owned field code, port_gen draft, port_check PASS.
// gen_func_<addr>=oracle leg; register-literal faithful drafts (rename pass deferred).
#include "core.h"
#include "override_registry.h"
void rec_dispatch(Core*, uint32_t);
extern void shard_set_override(uint32_t, void (*)(Core*));

void func_8001CF2C(Core*);
void func_8001DC40(Core*);
void func_80022D08(Core*);
void func_80023528(Core*);
void func_80024548(Core*);
void func_80024E00(Core*);
void func_80025744(Core*);
void func_80025934(Core*);
void func_80025B78(Core*);
void func_80025D98(Core*);
void func_80027768(Core*);
void func_80028E10(Core*);
void func_8002AE0C(Core*);
void func_8002F514(Core*);
void func_800310F4(Core*);
void func_8003116C(Core*);
void func_800317CC(Core*);
void func_800329E0(Core*);
void func_80033AFC(Core*);
void func_8003A1E4(Core*);
void func_8003A290(Core*);
void func_8003A3E8(Core*);
void func_8003A470(Core*);
void func_8003A5E4(Core*);
void func_8003B588(Core*);
void func_8003BB50(Core*);
void func_8003BCF4(Core*);
void func_8003BF00(Core*);
void func_8003C048(Core*);
void func_8003CCA4(Core*);
void func_8003D23C(Core*);
void func_8003E030(Core*);
void func_8003E448(Core*);
void func_8003EEC0(Core*);
void func_8003F024(Core*);
void func_80040AA4(Core*);
void func_80040CDC(Core*);
void func_80041718(Core*);
void func_80045558(Core*);
void func_80045580(Core*);
void func_800455C0(Core*);
void func_80045724(Core*);
void func_80045810(Core*);
void func_800459D0(Core*);
void func_80045CAC(Core*);
void func_8004602C(Core*);
void func_800462E4(Core*);
void func_800468AC(Core*);
void func_80046A44(Core*);
void func_8004766C(Core*);
void func_80047778(Core*);
void func_80047CBC(Core*);
void func_80048360(Core*);
void func_80048654(Core*);
void func_80048750(Core*);
void func_80048B30(Core*);
void func_80048ECC(Core*);
void func_80048FC4(Core*);
void func_800490E4(Core*);
void func_80049250(Core*);
void func_80049280(Core*);
void func_800492B0(Core*);
void func_800493E8(Core*);
void func_80049418(Core*);
void func_80049674(Core*);
void func_80049760(Core*);
void func_80049800(Core*);
void func_800498C8(Core*);
void func_80049968(Core*);
void func_80049F80(Core*);
void func_8004BD04(Core*);
void func_8004C0E4(Core*);
void func_8004D4C4(Core*);
void func_8004D514(Core*);
void func_8004D7EC(Core*);
void func_8004D868(Core*);
void func_8004EA4C(Core*);
void func_8004EAD0(Core*);
void func_8004ED94(Core*);
void func_8004EE2C(Core*);
void func_8004EE50(Core*);
void func_8004EE88(Core*);
void func_8004EF54(Core*);
void func_8004EF8C(Core*);
void func_8004EFC0(Core*);
void func_8004F058(Core*);
void func_8004F184(Core*);
void func_8004F378(Core*);
void func_8004FA38(Core*);
void func_8004FD30(Core*);
void func_8005019C(Core*);
void func_80051794(Core*);
void func_800517BC(Core*);
void func_800519E0(Core*);
void func_80051F14(Core*);
void func_80052010(Core*);
void func_800521F4(Core*);
void func_800525D0(Core*);
void func_8005314C(Core*);
void func_800532A0(Core*);
void func_800535E0(Core*);
void func_800538E0(Core*);
void func_80053D0C(Core*);
void func_80053D90(Core*);
void func_800541F4(Core*);
void func_800543C0(Core*);
void func_8005444C(Core*);
void func_80054790(Core*);
void func_80054D14(Core*);
void func_80054E80(Core*);
void func_80055284(Core*);
void func_80055634(Core*);
void func_80055824(Core*);
void func_80055F48(Core*);
void func_80056D44(Core*);
void func_80056E08(Core*);
void func_80056EC8(Core*);
void func_80056F3C(Core*);
void func_8005706C(Core*);
void func_80057150(Core*);
void func_800572EC(Core*);
void func_8005749C(Core*);
void func_80057A68(Core*);
void func_80058648(Core*);
void func_800597AC(Core*);
void func_80067DA8(Core*);
void func_80067EF4(Core*);
void func_80067FE4(Core*);
void func_8006CE74(Core*);
void func_80072114(Core*);
void func_80072DDC(Core*);
void func_80073750(Core*);
void func_80074590(Core*);
void func_80074BF8(Core*);
void func_800750D8(Core*);
void func_800753AC(Core*);
void func_800753D4(Core*);
void func_80075D58(Core*);
void func_80075FF8(Core*);
void func_80076D68(Core*);
void func_800776F8(Core*);
void func_8007778C(Core*);
void func_80077B38(Core*);
void func_80077B5C(Core*);
void func_80077C40(Core*);
void func_80077CFC(Core*);
void func_80077D64(Core*);
void func_80077E3C(Core*);
void func_800782B0(Core*);
void func_80078798(Core*);
void func_80078988(Core*);
void func_80078CA8(Core*);
void func_80079324(Core*);
void func_80079374(Core*);
void func_800793C4(Core*);
void func_80079528(Core*);
void func_8007982C(Core*);
void func_8007A980(Core*);
void func_8007B2C0(Core*);
void func_8007B38C(Core*);
void func_8007BE18(Core*);
void func_8007E1B8(Core*);
void func_8007E6DC(Core*);
void func_8007E8DC(Core*);
void func_8007E938(Core*);
void func_8007E998(Core*);
void func_8007F078(Core*);
void func_8007F104(Core*);
void func_8007F250(Core*);
void func_8007F498(Core*);
void func_8007F73C(Core*);
void func_8007F8F8(Core*);
void func_8007FC24(Core*);
void func_8007FCC8(Core*);
void func_80081CF8(Core*);
void func_80083C30(Core*);
void func_80083C70(Core*);
void func_80083CA0(Core*);
void func_80083CC0(Core*);
void func_80083DB0(Core*);
void func_80083DE0(Core*);
void func_80083E80(Core*);
void func_80083F50(Core*);
void func_80084080(Core*);
void func_80084110(Core*);
void func_80084220(Core*);
void func_800844C0(Core*);
void func_80084520(Core*);
void func_800847F0(Core*);
void func_80084D10(Core*);
void func_80084EB0(Core*);
void func_80085050(Core*);
void func_80085480(Core*);
void func_80085690(Core*);
void func_80087AEC(Core*);
void func_80087E2C(Core*);
void func_80087EAC(Core*);
void func_80089F68(Core*);
void func_8008DD7C(Core*);
void func_80091AF0(Core*);
void func_80096380(Core*);
void func_80096390(Core*);
void func_800963A0(Core*);
void func_800963D0(Core*);
void func_80098F90(Core*);
void func_8009A420(Core*);
void func_8009A450(Core*);
void func_8009A480(Core*);
void func_8009A600(Core*);
void func_8009B0C0(Core*);

extern void gen_func_80023528(Core*);
extern void gen_func_800248D0(Core*);
extern void gen_func_80024E00(Core*);
extern void gen_func_80024F18(Core*);
extern void gen_func_80025D98(Core*);
extern void gen_func_80028E10(Core*);
extern void gen_func_8002AE0C(Core*);
extern void gen_func_8002F514(Core*);
extern void gen_func_800317CC(Core*);
extern void gen_func_800329E0(Core*);
extern void gen_func_80033AFC(Core*);
extern void gen_func_8003A290(Core*);
extern void gen_func_8003A3E8(Core*);
extern void gen_func_8003A470(Core*);
extern void gen_func_8003A5E4(Core*);
extern void gen_func_8003A790(Core*);
extern void gen_func_8003A9A0(Core*);
extern void gen_func_8003ABE4(Core*);
extern void gen_func_8003B588(Core*);
extern void gen_func_8003E030(Core*);
extern void gen_func_8003E264(Core*);
extern void gen_func_8003E448(Core*);
extern void gen_func_8003E894(Core*);
extern void gen_func_8003EA88(Core*);
extern void gen_func_8003EBE0(Core*);
extern void gen_func_8003F024(Core*);
extern void gen_func_8003FA1C(Core*);
extern void gen_func_8003FA44(Core*);
extern void gen_func_8003FB84(Core*);
extern void gen_func_8003FB94(Core*);
extern void gen_func_8003FBC4(Core*);
extern void gen_func_8003FC00(Core*);
extern void gen_func_8003FC78(Core*);
extern void gen_func_8003FC8C(Core*);
extern void gen_func_8003FE00(Core*);
extern void gen_func_8003FED8(Core*);
extern void gen_func_80041768(Core*);
extern void gen_func_80044CD4(Core*);
extern void gen_func_80045CAC(Core*);
extern void gen_func_800468AC(Core*);
extern void gen_func_80047778(Core*);
extern void gen_func_80047B5C(Core*);
extern void gen_func_80048654(Core*);
extern void gen_func_800489E4(Core*);
extern void gen_func_80048B30(Core*);
extern void gen_func_80048ECC(Core*);
extern void gen_func_80048FC4(Core*);
extern void gen_func_80049250(Core*);
extern void gen_func_80049280(Core*);
extern void gen_func_8004954C(Core*);
extern void gen_func_80049674(Core*);
extern void gen_func_80049760(Core*);
extern void gen_func_80049F80(Core*);
extern void gen_func_8004A118(Core*);
extern void gen_func_8004A2A0(Core*);
extern void gen_func_8004B428(Core*);
extern void gen_func_8004C0E4(Core*);
extern void gen_func_8004CBD8(Core*);
extern void gen_func_8004D514(Core*);
extern void gen_func_8004D650(Core*);
extern void gen_func_8004D714(Core*);
extern void gen_func_8004D79C(Core*);
extern void gen_func_8004D8B0(Core*);
extern void gen_func_8004DAEC(Core*);
extern void gen_func_8004ED0C(Core*);
extern void gen_func_8004EE88(Core*);
extern void gen_func_8004EF54(Core*);
extern void gen_func_8004EF8C(Core*);
extern void gen_func_8004F058(Core*);
extern void gen_func_8004F378(Core*);
extern void gen_func_8004F430(Core*);
extern void gen_func_8004F474(Core*);
extern void gen_func_8004F514(Core*);
extern void gen_func_8004F6D0(Core*);
extern void gen_func_80050894(Core*);
extern void gen_func_800521F4(Core*);
extern void gen_func_8005245C(Core*);
extern void gen_func_800525D0(Core*);
extern void gen_func_8005262C(Core*);
extern void gen_func_80052694(Core*);
extern void gen_func_80052720(Core*);
extern void gen_func_8005314C(Core*);
extern void gen_func_800532A0(Core*);
extern void gen_func_800538E0(Core*);
extern void gen_func_80053D0C(Core*);
extern void gen_func_80053D90(Core*);
extern void gen_func_800541F4(Core*);
extern void gen_func_800543C0(Core*);
extern void gen_func_8005444C(Core*);
extern void gen_func_80054E80(Core*);
extern void gen_func_800551C4(Core*);
extern void gen_func_80055284(Core*);
extern void gen_func_80055634(Core*);
extern void gen_func_80055824(Core*);
extern void gen_func_80055D5C(Core*);
extern void gen_func_80055E28(Core*);
extern void gen_func_80055F48(Core*);
extern void gen_func_80055FBC(Core*);
extern void gen_func_80056C00(Core*);
extern void gen_func_80056D44(Core*);
extern void gen_func_80056E08(Core*);
extern void gen_func_80056F3C(Core*);
extern void gen_func_8005706C(Core*);
extern void gen_func_800572EC(Core*);
extern void gen_func_8005749C(Core*);
extern void gen_func_800574E0(Core*);
extern void gen_func_80057A68(Core*);
extern void gen_func_80057C08(Core*);
extern void gen_func_80059C60(Core*);
extern void gen_func_8005A714(Core*);
extern void gen_func_80062D8C(Core*);
extern void gen_func_80067EF4(Core*);
extern void gen_func_80067FE4(Core*);
extern void gen_func_80068214(Core*);
extern void gen_func_800682C4(Core*);
extern void gen_func_8006CE74(Core*);
extern void gen_func_8006CEC4(Core*);
extern void gen_func_8006F138(Core*);
extern void gen_func_800708B4(Core*);
extern void gen_func_800716B4(Core*);
extern void gen_func_800737F8(Core*);
extern void gen_func_800738B0(Core*);
extern void gen_func_8007413C(Core*);
extern void gen_func_80074B44(Core*);
extern void gen_func_80074E48(Core*);
extern void gen_func_800753AC(Core*);
extern void gen_func_8007566C(Core*);
extern void gen_func_80075D58(Core*);
extern void gen_func_80075FF8(Core*);
extern void gen_func_800776F8(Core*);
extern void gen_func_800782B0(Core*);
extern void gen_func_80078798(Core*);
extern void gen_func_80078824(Core*);
extern void gen_func_800793C4(Core*);
extern void gen_func_80079464(Core*);
extern void gen_func_8007982C(Core*);
extern void gen_func_8007A810(Core*);
extern void gen_func_8007A8E0(Core*);
extern void gen_func_8007B0F0(Core*);
extern void gen_func_8007B38C(Core*);
extern void gen_func_8007B45C(Core*);
extern void gen_func_8007BE18(Core*);
extern void gen_func_8007BF20(Core*);
extern void gen_func_8007E8DC(Core*);
extern void gen_func_8007E998(Core*);
extern void gen_func_8007ED5C(Core*);
extern void gen_func_8007EE74(Core*);
extern void gen_func_8007EF60(Core*);
extern void gen_func_8007F078(Core*);
extern void gen_func_8007F104(Core*);
extern void gen_func_8007F250(Core*);
extern void gen_func_8007F498(Core*);
extern void gen_func_8007F73C(Core*);
extern void gen_func_8007F8F8(Core*);
extern void gen_func_8007FD54(Core*);
extern void gen_func_80022D08(Core*);
extern void gen_func_80025744(Core*);
extern void gen_func_80025934(Core*);
extern void gen_func_80025B78(Core*);
extern void gen_func_80027768(Core*);
extern void gen_func_8003A1E4(Core*);
extern void gen_func_8003D23C(Core*);
extern void gen_func_800455C0(Core*);
extern void gen_func_8004602C(Core*);
extern void gen_func_800490E4(Core*);
extern void gen_func_800492B0(Core*);
extern void gen_func_800493E8(Core*);
extern void gen_func_8004EAD0(Core*);
extern void gen_func_8004EE2C(Core*);
extern void gen_func_8004EE50(Core*);
extern void gen_func_8004F184(Core*);
extern void gen_func_800535E0(Core*);
extern void gen_func_80054790(Core*);
extern void gen_func_80056EC8(Core*);
extern void gen_func_80057150(Core*);
extern void gen_func_80072114(Core*);
extern void gen_func_80077D64(Core*);
extern void gen_func_80077E3C(Core*);
extern void gen_func_80079324(Core*);
extern void gen_func_8007E6DC(Core*);
extern void gen_func_8007E938(Core*);
extern void gen_func_8007FC24(Core*);
extern void gen_func_8007FCC8(Core*);
extern void gen_func_80045724(Core*);
extern void gen_func_800462E4(Core*);
extern void gen_func_80049418(Core*);
extern void gen_func_8004EFC0(Core*);
extern void gen_func_80045810(Core*);
extern void gen_func_80049800(Core*);
extern void gen_func_800459D0(Core*);

namespace {
static void leaf_80023528(Core* c) {
    c->r[2] = c->r[0] + (uint32_t)3;
    c->mem_w8((c->r[4] + (uint32_t)0), (uint8_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)2;
    c->mem_w8((c->r[4] + (uint32_t)4), (uint8_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)11;
    c->mem_w8((c->r[4] + (uint32_t)5), (uint8_t)c->r[2]);
    c->mem_w8((c->r[4] + (uint32_t)6), (uint8_t)c->r[0]); return;
    return;
}

static void leaf_800248D0(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-40;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
    c->r[18] = c->r[4] + c->r[0];
    c->r[2] = (uint32_t)32780u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-1933));
    c->r[6] = c->r[0] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)36), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[20]);
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[19]);
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    { int _t = (c->r[2] != c->r[0]); c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]); if (_t) goto L_800249C0; }
    c->r[2] = (uint32_t)8064u << 16;
    c->r[4] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)326));
    c->r[17] = c->mem_r32((c->r[4] + (uint32_t)320));
    c->r[5] = (uint32_t)8064u << 16;
    c->mem_w8((c->r[5] + (uint32_t)386), (uint8_t)c->r[3]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)382));
    c->r[3] = c->r[3] & 255u;
    c->r[2] = c->r[2] & 15u;
    { int _t = (c->r[3] == c->r[0]); c->r[19] = c->r[2] + (uint32_t)-3; if (_t) goto L_80024ACC; }
    c->r[18] = c->r[5] + c->r[0];
    c->r[2] = (uint32_t)32769u << 16;
    c->r[20] = c->r[2] + (uint32_t)29476;
  L_80024938:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)386));
    c->r[16] = c->mem_r32((c->r[17] + (uint32_t)0));
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->mem_w8((c->r[18] + (uint32_t)386), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)2));
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)101);
    { int _t = (c->r[2] == c->r[0]); c->r[17] = c->r[17] + (uint32_t)4; if (_t) goto L_800249A4; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)2));
    c->r[3] = c->r[2] >> 3;
    c->r[3] = c->r[3] + c->r[20];
    c->r[3] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)0));
    c->r[2] = c->r[2] & 7u;
    c->r[3] = (uint32_t)((int32_t)c->r[3] >> (c->r[2] & 31));
    c->r[3] = c->r[3] & 1u;
    { int _t = (c->r[3] == c->r[0]); c->r[5] = (uint32_t)8064u << 16; if (_t) goto L_800249A8; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)0));
    c->r[2] = c->r[2] & 1u;
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[19] & 255u; if (_t) goto L_800249A8; }
    c->r[31] = 0x800249A0u;
    c->r[5] = c->r[16] + c->r[0]; func_8002F514(c);
    c->r[6] = c->r[0] + (uint32_t)1;
  L_800249A4:;
    c->r[5] = (uint32_t)8064u << 16;
  L_800249A8:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)386));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[6] + c->r[0]; if (_t) goto L_80024938; }
     goto L_80024AD0;
  L_800249C0:;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[4] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)338));
    c->r[17] = c->mem_r32((c->r[4] + (uint32_t)332));
    c->r[5] = (uint32_t)8064u << 16;
    c->mem_w8((c->r[5] + (uint32_t)386), (uint8_t)c->r[3]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)382));
    c->r[3] = c->r[3] & 255u;
    c->r[2] = c->r[2] & 15u;
    { int _t = (c->r[3] == c->r[0]); c->r[19] = c->r[2] + (uint32_t)-3; if (_t) goto L_80024ACC; }
    c->r[20] = c->r[5] + c->r[0];
  L_800249F0:;
    c->r[16] = c->mem_r32((c->r[17] + (uint32_t)0));
    c->r[17] = c->r[17] + (uint32_t)4;
    c->r[5] = (uint32_t)255u << 16;
    c->r[5] = c->r[5] | 255u;
    c->r[4] = (uint32_t)103u << 16;
    c->r[4] = c->r[4] | 1u;
    c->r[2] = (uint32_t)c->mem_r8((c->r[20] + (uint32_t)386));
    c->r[3] = c->mem_r32((c->r[16] + (uint32_t)0));
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->r[3] = c->r[3] & c->r[5];
    { int _t = (c->r[3] != c->r[4]); c->mem_w8((c->r[20] + (uint32_t)386), (uint8_t)c->r[2]); if (_t) goto L_80024ABC; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[18] + (uint32_t)46));
    c->r[3] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)46));
    c->r[2] = c->r[2] - c->r[3];
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    { int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[2]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[2] = (uint32_t)c->mem_r16((c->r[18] + (uint32_t)54));
    c->r[3] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)54));
    c->r[2] = c->r[2] - c->r[3];
    c->r[4] = c->lo;
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    { int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[2]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[3] = c->lo;
    c->r[31] = 0x80024A68u;
    c->r[4] = c->r[4] + c->r[3]; func_80084080(c);
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[18] + (uint32_t)128));
    c->r[2] = c->r[2] & 65535u;
    c->r[3] = c->r[3] + (uint32_t)700;
    c->r[3] = (uint32_t)((int32_t)c->r[3] < (int32_t)c->r[2]);
    { int _t = (c->r[3] != c->r[0]);  if (_t) goto L_80024AD0; }
    c->r[3] = (uint32_t)c->mem_r16((c->r[18] + (uint32_t)50));
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)50));
    c->r[4] = (uint32_t)c->mem_r16((c->r[18] + (uint32_t)132));
    c->r[3] = c->r[3] - c->r[2];
    c->r[3] = c->r[3] + (uint32_t)420;
    c->r[4] = c->r[4] + c->r[3];
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[18] + (uint32_t)134));
    c->r[4] = c->r[4] & 65535u;
    c->r[2] = c->r[2] + (uint32_t)520;
    c->r[2] = (uint32_t)((int32_t)c->r[2] < (int32_t)c->r[4]);
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[19] & 255u; if (_t) goto L_80024AD0; }
    c->r[31] = 0x80024AB8u;
    c->r[5] = c->r[16] + c->r[0]; func_8002F514(c);
    c->r[6] = c->r[0] + (uint32_t)1;
  L_80024ABC:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[20] + (uint32_t)386));
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_800249F0; }
  L_80024ACC:;
    c->r[2] = c->r[6] + c->r[0];
  L_80024AD0:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)36));
    c->r[20] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)40; return;
    return;
}

static void leaf_80024E00(Core* c) {
    c->r[2] = (uint32_t)32782u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)32495));
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->r[3] = c->r[2] + (uint32_t)-18;
    c->r[2] = (uint32_t)(c->r[3] < (uint32_t)5);
    { int _t = (c->r[2] == c->r[0]); c->mem_w32((c->r[29] + (uint32_t)20), c->r[31]); if (_t) goto L_80024EE4; }
    c->r[2] = (uint32_t)32769u << 16;
    c->r[2] = c->r[2] + (uint32_t)404;
    c->r[3] = c->r[3] << 2;
    c->r[3] = c->r[3] + c->r[2];
    c->r[2] = c->mem_r32((c->r[3] + (uint32_t)0));
    {  switch (c->r[2]) {  default: rec_dispatch(c, c->r[2]); return; } }
    return;
  L_80024EE4:;
    c->r[4] = c->r[16] + c->r[0];
    c->r[5] = (uint32_t)32769u << 16;
    c->r[5] = c->r[5] + (uint32_t)29492;
    c->r[31] = 0x80024EF8u;
    c->r[6] = c->r[0] + (uint32_t)36; func_80077B38(c);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)13));
    c->r[2] = c->r[2] & 127u;
    c->mem_w8((c->r[16] + (uint32_t)13), (uint8_t)c->r[2]);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_80024F18(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->r[3] = (uint32_t)32782u << 16;
    c->r[2] = (uint32_t)32780u << 16;
    c->r[4] = c->r[2] + (uint32_t)-1936;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[3] + (uint32_t)32750));
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[31]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)13));
    c->r[2] = (uint32_t)((int32_t)c->r[2] < (int32_t)c->r[3]);
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80024F80; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)492));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[2] + (uint32_t)-1; if (_t) goto L_80024F68; }
    c->r[2] = (uint32_t)32783u << 16;
    c->mem_w8((c->r[2] + (uint32_t)-12191), (uint8_t)c->r[0]); goto L_80024F98;
  L_80024F68:;
    c->mem_w8((c->r[4] + (uint32_t)492), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)9));
    c->r[2] = c->r[2] | 1u;
    c->mem_w8((c->r[16] + (uint32_t)9), (uint8_t)c->r[2]); goto L_80024F98;
  L_80024F80:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)9));
    c->r[2] = c->r[2] | 1u;
    c->mem_w8((c->r[16] + (uint32_t)9), (uint8_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)45;
    c->mem_w8((c->r[4] + (uint32_t)492), (uint8_t)c->r[2]);
  L_80024F98:;
    c->r[3] = (uint32_t)65280u << 16;
    c->r[2] = (uint32_t)32780u << 16;
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)-2036));
    c->r[3] = c->r[3] | 65280u;
    c->r[2] = c->r[2] & c->r[3];
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)32780u << 16; if (_t) goto L_80024FD4; }
    c->r[3] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-1936));
    c->r[2] = c->r[0] + (uint32_t)3;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_80024FD4; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)311));
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_80024FDC; }
  L_80024FD4:;
    c->mem_w8((c->r[16] + (uint32_t)1), (uint8_t)c->r[0]); goto L_80024FE0;
  L_80024FDC:;
    c->mem_w8((c->r[16] + (uint32_t)1), (uint8_t)c->r[2]);
  L_80024FE0:;
    c->r[2] = (uint32_t)32780u << 16;
    c->r[3] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-1762));
    c->r[2] = c->r[0] + (uint32_t)255;
    { int _t = (c->r[3] != c->r[2]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_80024FFC; }
    c->mem_w8((c->r[16] + (uint32_t)10), (uint8_t)c->r[2]); goto L_80025000;
  L_80024FFC:;
    c->mem_w8((c->r[16] + (uint32_t)10), (uint8_t)c->r[0]);
  L_80025000:;
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)3));
    c->r[4] = c->r[0] + (uint32_t)1;
    { int _t = (c->r[3] == c->r[4]); c->r[2] = (uint32_t)((int32_t)c->r[3] < 2); if (_t) goto L_8002507C; }
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80025028; }
    { int _t = (c->r[3] == c->r[0]); c->r[2] = (uint32_t)32780u << 16; if (_t) goto L_8002503C; }
     goto L_800251E0;
  L_80025028:;
    c->r[2] = c->r[0] + (uint32_t)2;
    { int _t = (c->r[3] == c->r[2]);  if (_t) goto L_8002516C; }
     goto L_800251E0;
  L_8002503C:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-2016));
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80025060; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)13));
    c->mem_w8((c->r[16] + (uint32_t)3), (uint8_t)c->r[4]);
    c->r[2] = c->r[2] | 1u;
    c->mem_w8((c->r[16] + (uint32_t)13), (uint8_t)c->r[2]); goto L_800251E0;
  L_80025060:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)13));
    c->r[2] = c->r[2] & 1u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)2; if (_t) goto L_800251E0; }
    c->mem_w8((c->r[16] + (uint32_t)3), (uint8_t)c->r[2]); goto L_800251E0;
  L_8002507C:;
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)4));
    { int _t = (c->r[3] == c->r[4]); c->r[2] = (uint32_t)((int32_t)c->r[3] < 2); if (_t) goto L_800250D4; }
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_800250A4; }
    { int _t = (c->r[3] == c->r[0]); c->r[3] = c->r[0] + c->r[0]; if (_t) goto L_800250B8; }
     goto L_80025154;
  L_800250A4:;
    c->r[2] = c->r[0] + (uint32_t)2;
    { int _t = (c->r[3] == c->r[2]); c->r[3] = c->r[0] + c->r[0]; if (_t) goto L_80025120; }
     goto L_80025154;
  L_800250B8:;
    c->mem_w8((c->r[16] + (uint32_t)4), (uint8_t)c->r[4]);
    c->r[4] = c->r[0] + (uint32_t)11;
    c->r[31] = 0x800250C8u;
    c->r[5] = c->r[0] + (uint32_t)64; func_8004ED94(c);
    c->r[2] = c->r[0] + (uint32_t)16;
    c->mem_w16((c->r[16] + (uint32_t)16), (uint16_t)c->r[2]); goto L_80025150;
  L_800250D4:;
    c->r[2] = (uint32_t)32780u << 16;
    c->r[2] = c->r[2] + (uint32_t)-1936;
    c->r[3] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)15));
    c->r[3] = c->r[3] + (uint32_t)1;
    c->mem_w8((c->r[2] + (uint32_t)15), (uint8_t)c->r[3]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)16));
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->mem_w16((c->r[16] + (uint32_t)16), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    { int _t = (c->r[2] != c->r[0]); c->r[3] = c->r[0] + c->r[0]; if (_t) goto L_80025154; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)4));
    c->r[3] = c->r[0] + (uint32_t)60;
    c->mem_w16((c->r[16] + (uint32_t)16), (uint16_t)c->r[3]);
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w8((c->r[16] + (uint32_t)4), (uint8_t)c->r[2]); goto L_80025150;
  L_80025120:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)16));
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->mem_w16((c->r[16] + (uint32_t)16), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80025154; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)13));
    c->r[3] = c->r[0] + (uint32_t)1;
    c->r[2] = c->r[2] & 254u;
    c->mem_w8((c->r[16] + (uint32_t)13), (uint8_t)c->r[2]); goto L_80025154;
  L_80025150:;
    c->r[3] = c->r[0] + c->r[0];
  L_80025154:;
    { int _t = (c->r[3] == c->r[0]); c->r[2] = (uint32_t)32780u << 16; if (_t) goto L_800251E0; }
    c->mem_w8((c->r[16] + (uint32_t)3), (uint8_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)4), (uint8_t)c->r[0]);
    c->mem_w8((c->r[2] + (uint32_t)-2016), (uint8_t)c->r[0]); goto L_800251E0;
  L_8002516C:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)4));
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_8002518C; }
    { int _t = (c->r[2] == c->r[4]); c->r[3] = c->r[0] + c->r[0]; if (_t) goto L_8002519C; }
     goto L_800251D0;
  L_8002518C:;
    c->r[2] = c->r[0] + (uint32_t)45;
    c->mem_w8((c->r[16] + (uint32_t)4), (uint8_t)c->r[4]);
    c->mem_w16((c->r[16] + (uint32_t)18), (uint16_t)c->r[2]); goto L_800251CC;
  L_8002519C:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)18));
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->mem_w16((c->r[16] + (uint32_t)18), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_800251D0; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)13));
    c->r[3] = c->r[0] + (uint32_t)1;
    c->r[2] = c->r[2] & 254u;
    c->mem_w8((c->r[16] + (uint32_t)13), (uint8_t)c->r[2]); goto L_800251D0;
  L_800251CC:;
    c->r[3] = c->r[0] + c->r[0];
  L_800251D0:;
    { int _t = (c->r[3] == c->r[0]);  if (_t) goto L_800251E0; }
    c->mem_w8((c->r[16] + (uint32_t)3), (uint8_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)4), (uint8_t)c->r[0]);
  L_800251E0:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_80025D98(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->r[2] = (uint32_t)32783u << 16;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[2] + (uint32_t)-12200;
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[31]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)13));
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)32780u << 16; if (_t) goto L_80025E24; }
    c->r[4] = c->r[2] + (uint32_t)-1936;
    c->r[3] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-1936));
    c->r[2] = c->r[0] + (uint32_t)5;
    { int _t = (c->r[3] != c->r[2]); c->r[2] = c->r[0] + (uint32_t)3; if (_t) goto L_80025DE8; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)1));
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)3);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)3; if (_t) goto L_80025E24; }
  L_80025DE8:;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = c->r[0] + (uint32_t)20; if (_t) goto L_80025E24; }
    { int _t = (c->r[3] == c->r[2]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_80025E24; }
    c->r[3] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)311));
    c->r[2] = c->r[0] + (uint32_t)1;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = (uint32_t)32780u << 16; if (_t) goto L_80025E24; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-2014));
    c->r[2] = c->r[2] & 4u;
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80025E24; }
    c->r[31] = 0x80025E24u;
    c->r[4] = c->r[16] + c->r[0]; func_80025744(c);
  L_80025E24:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)9));
    c->r[3] = c->r[2] & 3u;
    c->r[2] = c->r[0] + (uint32_t)1;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = (uint32_t)((int32_t)c->r[3] < 2); if (_t) goto L_80025E54; }
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)3; if (_t) goto L_80025EB8; }
    { int _t = (c->r[3] == c->r[2]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_80025EAC; }
     goto L_80025EB8;
  L_80025E54:;
    c->r[2] = (uint32_t)32780u << 16;
    c->r[4] = c->r[2] + (uint32_t)-1936;
    c->r[3] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-1936));
    c->r[2] = c->r[0] + (uint32_t)5;
    { int _t = (c->r[3] != c->r[2]); c->r[2] = c->r[0] + (uint32_t)2; if (_t) goto L_80025E84; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)1));
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)3);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)2; if (_t) goto L_80025EB8; }
  L_80025E84:;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = c->r[0] + (uint32_t)7; if (_t) goto L_80025EB8; }
    { int _t = (c->r[3] == c->r[2]); c->r[2] = c->r[0] + (uint32_t)20; if (_t) goto L_80025EB8; }
    { int _t = (c->r[3] == c->r[2]);  if (_t) goto L_80025EB8; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)1));
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_80025FB8; }
  L_80025EAC:;
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x80025EB8u;
    c->r[6] = c->r[5] + c->r[0]; func_80025934(c);
  L_80025EB8:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)1));
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)32780u << 16; if (_t) goto L_80025FB8; }
    c->r[3] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-1936));
    c->r[2] = c->r[0] + (uint32_t)3;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = (uint32_t)((int32_t)c->r[3] < 4); if (_t) goto L_80025FB8; }
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)2; if (_t) goto L_80025EF0; }
    { int _t = (c->r[3] == c->r[2]); c->r[2] = (uint32_t)32780u << 16; if (_t) goto L_80025F0C; }
    c->r[3] = c->r[2] + (uint32_t)-1936; goto L_80025F74;
  L_80025EF0:;
    c->r[2] = c->r[0] + (uint32_t)7;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = c->r[0] + (uint32_t)20; if (_t) goto L_80025F40; }
    { int _t = (c->r[3] == c->r[2]); c->r[2] = (uint32_t)32780u << 16; if (_t) goto L_80025FB8; }
    c->r[3] = c->r[2] + (uint32_t)-1936; goto L_80025F74;
  L_80025F0C:;
    c->r[2] = (uint32_t)32780u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-2026));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_80025FBC; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)8));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_80025FBC; }
    c->r[31] = 0x80025F38u;
    c->r[4] = c->r[16] + c->r[0]; rec_dispatch(c, 0x80113628u);
    c->r[2] = (uint32_t)8064u << 16; goto L_80025FBC;
  L_80025F40:;
    c->r[2] = (uint32_t)32780u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-2026));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_80025FBC; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)8));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_80025FBC; }
    c->r[31] = 0x80025F6Cu;
    c->r[4] = c->r[16] + c->r[0]; rec_dispatch(c, 0x801140A0u);
    c->r[2] = (uint32_t)8064u << 16; goto L_80025FBC;
  L_80025F74:;
    c->r[2] = c->mem_r32((c->r[3] + (uint32_t)16));
    c->r[2] = c->r[2] & 1536u;
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_80025FBC; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)17));
    c->r[2] = c->r[2] & 1u;
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_80025FBC; }
    c->r[2] = (uint32_t)32780u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-2026));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_80025FBC; }
    c->r[31] = 0x80025FB8u;
    c->r[4] = c->r[16] + c->r[0]; func_80025B78(c);
  L_80025FB8:;
    c->r[2] = (uint32_t)8064u << 16;
  L_80025FBC:;
    c->r[4] = c->mem_r32((c->r[2] + (uint32_t)312));
    c->r[3] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)76));
    c->r[2] = c->r[0] + (uint32_t)6;
    { int _t = (c->r[3] != c->r[2]);  if (_t) goto L_80026014; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)78));
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)32780u << 16; if (_t) goto L_80026014; }
    c->r[3] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-1936));
    c->r[2] = c->r[0] + (uint32_t)5;
    { int _t = (c->r[3] != c->r[2]); c->r[2] = c->r[0] + (uint32_t)20; if (_t) goto L_80026004; }
    c->r[31] = 0x80025FFCu;
     rec_dispatch(c, 0x801121ACu);
     goto L_80026014;
  L_80026004:;
    { int _t = (c->r[3] != c->r[2]);  if (_t) goto L_80026014; }
    c->r[31] = 0x80026014u;
     rec_dispatch(c, 0x8010F8CCu);
  L_80026014:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[31]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)20));
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_800260F0; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)21));
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_800260F0; }
    c->r[4] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)22));
    c->r[5] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)28));
    c->r[31] = 0x80026064u;
     func_80040AA4(c);
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)28));
    { int _t = (c->r[3] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_8002607C; }
    c->r[4] = c->r[0] + (uint32_t)2; goto L_80026084;
  L_8002607C:;
    { int _t = (c->r[3] != c->r[2]); c->r[4] = c->r[0] + (uint32_t)3; if (_t) goto L_8002608C; }
  L_80026084:;
    c->r[31] = 0x8002608Cu;
     func_80074BF8(c);
  L_8002608C:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)21));
    c->r[2] = c->r[2] + (uint32_t)-1;
    { int _t = ((int32_t)c->r[2] <= 0); c->r[3] = c->r[0] + c->r[0]; if (_t) goto L_800260D8; }
    c->r[5] = c->r[3] + (uint32_t)1;
  L_800260A4:;
    c->r[4] = c->r[16] + c->r[5];
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)22));
    c->r[3] = c->r[16] + c->r[3];
    c->mem_w8((c->r[3] + (uint32_t)22), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)28));
    c->mem_w8((c->r[3] + (uint32_t)28), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)21));
    c->r[3] = c->r[5] + c->r[0];
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->r[2] = (uint32_t)((int32_t)c->r[3] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]); c->r[5] = c->r[3] + (uint32_t)1; if (_t) goto L_800260A4; }
  L_800260D8:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)21));
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)20));
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->r[3] = c->r[3] + (uint32_t)1;
    c->mem_w8((c->r[16] + (uint32_t)21), (uint8_t)c->r[2]);
    c->mem_w8((c->r[16] + (uint32_t)20), (uint8_t)c->r[3]);
  L_800260F0:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_80028E10(Core* c) {
    c->r[7] = c->r[5] & 255u;
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 8);
    c->r[3] = c->r[5] + (uint32_t)-1;
    c->r[2] = (uint32_t)(c->r[3] < (uint32_t)22);
    { int _t = (c->r[2] == c->r[0]); c->r[6] = c->r[7] + c->r[0]; if (_t) goto L_80028E48; }
    c->r[2] = (uint32_t)32769u << 16;
    c->r[2] = c->r[2] + (uint32_t)540;
    c->r[3] = c->r[3] << 2;
    c->r[3] = c->r[3] + c->r[2];
    c->r[2] = c->mem_r32((c->r[3] + (uint32_t)0));
    {  switch (c->r[2]) {  default: rec_dispatch(c, c->r[2]); return; } }
  L_80028E48:;
    c->mem_w8((c->r[4] + (uint32_t)3), (uint8_t)c->r[6]);
    c->r[3] = c->r[7] << 1;
    c->r[3] = c->r[3] + c->r[7];
    c->r[3] = c->r[3] << 2;
    c->r[2] = (uint32_t)32778u << 16;
    c->r[2] = c->r[2] + (uint32_t)8372; goto L_80029120;
    return;
  L_80029120:;
    c->r[3] = c->r[3] + c->r[2];
    c->r[2] = c->r[0] + (uint32_t)32;
    c->mem_w8((c->r[4] + (uint32_t)11), (uint8_t)c->r[2]);
    c->r[2] = c->mem_r32((c->r[3] + (uint32_t)0));
    c->mem_w32((c->r[4] + (uint32_t)28), c->r[2]);
    c->r[2] = c->mem_r32((c->r[3] + (uint32_t)4));
    c->mem_w32((c->r[4] + (uint32_t)24), c->r[2]);
    c->r[2] = c->mem_r32((c->r[3] + (uint32_t)8));
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80029184; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)0));
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[2]);
    c->r[2] = c->mem_r32((c->r[3] + (uint32_t)8));
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)2));
    c->mem_w16((c->r[4] + (uint32_t)70), (uint16_t)c->r[2]);
    c->r[2] = c->mem_r32((c->r[3] + (uint32_t)8));
    c->r[2] = c->r[2] + (uint32_t)4;
    c->mem_w32((c->r[4] + (uint32_t)56), c->r[2]);
  L_80029184:;
     return;
    return;
}

static void leaf_8002AE0C(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-64;
    c->mem_w32((c->r[29] + (uint32_t)48), c->r[18]);
    c->r[18] = c->r[4] + c->r[0];
    c->r[7] = c->r[5] + c->r[0];
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w32((c->r[29] + (uint32_t)56), c->r[20]);
    c->r[20] = c->r[2] + (uint32_t)144;
    c->mem_w32((c->r[29] + (uint32_t)52), c->r[19]);
    c->r[19] = (uint32_t)8064u << 16;
    c->mem_w32((c->r[29] + (uint32_t)44), c->r[17]);
    c->r[17] = c->r[19] + (uint32_t)148;
    c->r[8] = c->r[0] + c->r[0];
    c->r[9] = c->r[8] + c->r[0];
    c->r[10] = c->r[8] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)60), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)40), c->r[16]);
    gte_write_ctrl(21, c->r[8]);
    gte_write_ctrl(22, c->r[9]);
    gte_write_ctrl(23, c->r[10]);
    c->r[2] = c->r[0] + (uint32_t)16;
    { int _t = (c->r[6] != c->r[0]); c->mem_w8((c->r[17] + (uint32_t)1), (uint8_t)c->r[2]); if (_t) goto L_8002B000; }
    c->r[4] = c->r[18] + (uint32_t)84;
    c->r[16] = (uint32_t)8064u << 16;
    c->r[16] = c->r[16] + (uint32_t)0;
    c->r[2] = (uint32_t)c->mem_r16((c->r[18] + (uint32_t)46));
    c->r[5] = c->r[16] + c->r[0];
    c->mem_w16((c->r[29] + (uint32_t)16), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[18] + (uint32_t)50));
    c->r[3] = (uint32_t)c->mem_r16((c->r[18] + (uint32_t)134));
    c->r[6] = (uint32_t)c->mem_r16((c->r[18] + (uint32_t)132));
    c->r[2] = c->r[2] + c->r[3];
    c->r[2] = c->r[2] - c->r[6];
    c->mem_w16((c->r[29] + (uint32_t)18), (uint16_t)c->r[2]);
    c->r[3] = (uint32_t)c->mem_r16((c->r[18] + (uint32_t)54));
    c->r[2] = (uint32_t)((int32_t)c->r[7] >> 3);
    c->mem_w8((c->r[19] + (uint32_t)148), (uint8_t)c->r[2]);
    c->mem_w8((c->r[17] + (uint32_t)2), (uint8_t)c->r[2]);
    c->r[31] = 0x8002AEACu;
    c->mem_w16((c->r[29] + (uint32_t)20), (uint16_t)c->r[3]); func_80085480(c);
    c->r[4] = c->r[16] + c->r[0];
    c->r[2] = (uint32_t)c->mem_r8((c->r[19] + (uint32_t)148));
    c->r[5] = c->r[29] + (uint32_t)24;
    c->r[2] = c->r[2] << 2;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)1));
    c->r[3] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)2));
    c->r[2] = c->r[2] << 2;
    c->r[3] = c->r[3] << 2;
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[2]);
    c->r[31] = 0x8002AEDCu;
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[3]); func_80084520(c);
    c->r[2] = c->r[29] + (uint32_t)16;
    c->r[8] = (uint32_t)8064u << 16;
    c->r[8] = c->r[8] + (uint32_t)248;
    c->r[12] = c->mem_r32((c->r[8] + (uint32_t)0));
    c->r[13] = c->mem_r32((c->r[8] + (uint32_t)4));
    gte_write_ctrl(0, c->r[12]);
    gte_write_ctrl(1, c->r[13]);
    c->r[12] = c->mem_r32((c->r[8] + (uint32_t)8));
    c->r[13] = c->mem_r32((c->r[8] + (uint32_t)12));
    c->r[14] = c->mem_r32((c->r[8] + (uint32_t)16));
    gte_write_ctrl(2, c->r[12]);
    gte_write_ctrl(3, c->r[13]);
    gte_write_ctrl(4, c->r[14]);
    c->r[12] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)0));
    c->r[13] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)6));
    c->r[14] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)12));
    gte_write_data(9, c->r[12]);
    gte_write_data(10, c->r[13]);
    gte_write_data(11, c->r[14]);
    gte_op(c, 0x4A49E012u);
    c->r[12] = gte_read_data(9);
    c->r[13] = gte_read_data(10);
    c->r[14] = gte_read_data(11);
    c->mem_w16((c->r[16] + (uint32_t)0), (uint16_t)c->r[12]);
    c->mem_w16((c->r[16] + (uint32_t)6), (uint16_t)c->r[13]);
    c->mem_w16((c->r[16] + (uint32_t)12), (uint16_t)c->r[14]);
    c->r[9] = (uint32_t)8064u << 16;
    c->r[9] = c->r[9] + (uint32_t)2;
    c->r[12] = (uint32_t)c->mem_r16((c->r[9] + (uint32_t)0));
    c->r[13] = (uint32_t)c->mem_r16((c->r[9] + (uint32_t)6));
    c->r[14] = (uint32_t)c->mem_r16((c->r[9] + (uint32_t)12));
    gte_write_data(9, c->r[12]);
    gte_write_data(10, c->r[13]);
    gte_write_data(11, c->r[14]);
    gte_op(c, 0x4A49E012u);
    c->r[10] = (uint32_t)8064u << 16;
    c->r[10] = c->r[10] + (uint32_t)2;
    c->r[12] = gte_read_data(9);
    c->r[13] = gte_read_data(10);
    c->r[14] = gte_read_data(11);
    c->mem_w16((c->r[10] + (uint32_t)0), (uint16_t)c->r[12]);
    c->mem_w16((c->r[10] + (uint32_t)6), (uint16_t)c->r[13]);
    c->mem_w16((c->r[10] + (uint32_t)12), (uint16_t)c->r[14]);
    c->r[8] = (uint32_t)8064u << 16;
    c->r[8] = c->r[8] + (uint32_t)4;
    c->r[12] = (uint32_t)c->mem_r16((c->r[8] + (uint32_t)0));
    c->r[13] = (uint32_t)c->mem_r16((c->r[8] + (uint32_t)6));
    c->r[14] = (uint32_t)c->mem_r16((c->r[8] + (uint32_t)12));
    gte_write_data(9, c->r[12]);
    gte_write_data(10, c->r[13]);
    gte_write_data(11, c->r[14]);
    gte_op(c, 0x4A49E012u);
    c->r[9] = (uint32_t)8064u << 16;
    c->r[9] = c->r[9] + (uint32_t)4;
    c->r[12] = gte_read_data(9);
    c->r[13] = gte_read_data(10);
    c->r[14] = gte_read_data(11);
    c->mem_w16((c->r[9] + (uint32_t)0), (uint16_t)c->r[12]);
    c->mem_w16((c->r[9] + (uint32_t)6), (uint16_t)c->r[13]);
    c->mem_w16((c->r[9] + (uint32_t)12), (uint16_t)c->r[14]);
    gte_write_data(0, c->mem_r32((c->r[2] + (uint32_t)0)));
    gte_write_data(1, c->mem_r32((c->r[2] + (uint32_t)4)));
    gte_op(c, 0x4A486012u);
    c->r[10] = (uint32_t)8064u << 16; goto L_8002B170;
  L_8002B000:;
    c->r[4] = (uint32_t)32778u << 16;
    c->r[4] = c->r[4] + (uint32_t)7372;
    c->r[16] = (uint32_t)8064u << 16;
    c->r[16] = c->r[16] + (uint32_t)0;
    c->r[5] = c->r[16] + c->r[0];
    c->r[2] = (uint32_t)((int32_t)c->r[7] >> 3);
    c->mem_w8((c->r[17] + (uint32_t)2), (uint8_t)c->r[2]);
    c->r[31] = 0x8002B024u;
    c->mem_w8((c->r[19] + (uint32_t)148), (uint8_t)c->r[2]); func_80085480(c);
    c->r[4] = c->r[16] + c->r[0];
    c->r[2] = (uint32_t)c->mem_r8((c->r[19] + (uint32_t)148));
    c->r[5] = c->r[29] + (uint32_t)24;
    c->r[2] = c->r[2] << 2;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)1));
    c->r[3] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)2));
    c->r[2] = c->r[2] << 2;
    c->r[3] = c->r[3] << 2;
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[2]);
    c->r[31] = 0x8002B054u;
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[3]); func_80084520(c);
    c->r[8] = (uint32_t)8064u << 16;
    c->r[8] = c->r[8] + (uint32_t)248;
    c->r[12] = c->mem_r32((c->r[8] + (uint32_t)0));
    c->r[13] = c->mem_r32((c->r[8] + (uint32_t)4));
    gte_write_ctrl(0, c->r[12]);
    gte_write_ctrl(1, c->r[13]);
    c->r[12] = c->mem_r32((c->r[8] + (uint32_t)8));
    c->r[13] = c->mem_r32((c->r[8] + (uint32_t)12));
    c->r[14] = c->mem_r32((c->r[8] + (uint32_t)16));
    gte_write_ctrl(2, c->r[12]);
    gte_write_ctrl(3, c->r[13]);
    gte_write_ctrl(4, c->r[14]);
    c->r[12] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)0));
    c->r[13] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)6));
    c->r[14] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)12));
    gte_write_data(9, c->r[12]);
    gte_write_data(10, c->r[13]);
    gte_write_data(11, c->r[14]);
    gte_op(c, 0x4A49E012u);
    c->r[12] = gte_read_data(9);
    c->r[13] = gte_read_data(10);
    c->r[14] = gte_read_data(11);
    c->mem_w16((c->r[16] + (uint32_t)0), (uint16_t)c->r[12]);
    c->mem_w16((c->r[16] + (uint32_t)6), (uint16_t)c->r[13]);
    c->mem_w16((c->r[16] + (uint32_t)12), (uint16_t)c->r[14]);
    c->r[9] = (uint32_t)8064u << 16;
    c->r[9] = c->r[9] + (uint32_t)2;
    c->r[12] = (uint32_t)c->mem_r16((c->r[9] + (uint32_t)0));
    c->r[13] = (uint32_t)c->mem_r16((c->r[9] + (uint32_t)6));
    c->r[14] = (uint32_t)c->mem_r16((c->r[9] + (uint32_t)12));
    gte_write_data(9, c->r[12]);
    gte_write_data(10, c->r[13]);
    gte_write_data(11, c->r[14]);
    gte_op(c, 0x4A49E012u);
    c->r[10] = (uint32_t)8064u << 16;
    c->r[10] = c->r[10] + (uint32_t)2;
    c->r[12] = gte_read_data(9);
    c->r[13] = gte_read_data(10);
    c->r[14] = gte_read_data(11);
    c->mem_w16((c->r[10] + (uint32_t)0), (uint16_t)c->r[12]);
    c->mem_w16((c->r[10] + (uint32_t)6), (uint16_t)c->r[13]);
    c->mem_w16((c->r[10] + (uint32_t)12), (uint16_t)c->r[14]);
    c->r[8] = (uint32_t)8064u << 16;
    c->r[8] = c->r[8] + (uint32_t)4;
    c->r[12] = (uint32_t)c->mem_r16((c->r[8] + (uint32_t)0));
    c->r[13] = (uint32_t)c->mem_r16((c->r[8] + (uint32_t)6));
    c->r[14] = (uint32_t)c->mem_r16((c->r[8] + (uint32_t)12));
    gte_write_data(9, c->r[12]);
    gte_write_data(10, c->r[13]);
    gte_write_data(11, c->r[14]);
    gte_op(c, 0x4A49E012u);
    c->r[9] = (uint32_t)8064u << 16;
    c->r[9] = c->r[9] + (uint32_t)4;
    c->r[12] = gte_read_data(9);
    c->r[13] = gte_read_data(10);
    c->r[14] = gte_read_data(11);
    c->mem_w16((c->r[9] + (uint32_t)0), (uint16_t)c->r[12]);
    c->mem_w16((c->r[9] + (uint32_t)6), (uint16_t)c->r[13]);
    c->mem_w16((c->r[9] + (uint32_t)12), (uint16_t)c->r[14]);
    gte_write_data(0, c->mem_r32((c->r[18] + (uint32_t)0)));
    gte_write_data(1, c->mem_r32((c->r[18] + (uint32_t)4)));
    gte_op(c, 0x4A486012u);
    c->r[10] = (uint32_t)8064u << 16;
  L_8002B170:;
    c->r[10] = c->r[10] + (uint32_t)20;
    c->mem_w32((c->r[10] + (uint32_t)0), gte_read_data(25));
    c->mem_w32((c->r[10] + (uint32_t)4), gte_read_data(26));
    c->mem_w32((c->r[10] + (uint32_t)8), gte_read_data(27));
    c->r[4] = (uint32_t)8064u << 16;
    c->r[4] = c->r[4] + (uint32_t)208;
    c->r[2] = c->mem_r32((c->r[16] + (uint32_t)20));
    c->r[3] = c->mem_r32((c->r[4] + (uint32_t)60));
    c->r[2] = c->r[2] + c->r[3];
    c->mem_w32((c->r[16] + (uint32_t)20), c->r[2]);
    c->r[2] = c->mem_r32((c->r[16] + (uint32_t)24));
    c->r[5] = c->mem_r32((c->r[4] + (uint32_t)64));
    c->r[3] = c->mem_r32((c->r[16] + (uint32_t)28));
    c->r[4] = c->mem_r32((c->r[4] + (uint32_t)68));
    c->r[2] = c->r[2] + c->r[5];
    c->r[3] = c->r[3] + c->r[4];
    c->mem_w32((c->r[16] + (uint32_t)24), c->r[2]);
    c->mem_w32((c->r[16] + (uint32_t)28), c->r[3]);
    c->r[12] = c->mem_r32((c->r[16] + (uint32_t)0));
    c->r[13] = c->mem_r32((c->r[16] + (uint32_t)4));
    gte_write_ctrl(0, c->r[12]);
    gte_write_ctrl(1, c->r[13]);
    c->r[12] = c->mem_r32((c->r[16] + (uint32_t)8));
    c->r[13] = c->mem_r32((c->r[16] + (uint32_t)12));
    c->r[14] = c->mem_r32((c->r[16] + (uint32_t)16));
    gte_write_ctrl(2, c->r[12]);
    gte_write_ctrl(3, c->r[13]);
    gte_write_ctrl(4, c->r[14]);
    c->r[12] = c->mem_r32((c->r[16] + (uint32_t)20));
    c->r[13] = c->mem_r32((c->r[16] + (uint32_t)24));
    gte_write_ctrl(5, c->r[12]);
    c->r[14] = c->mem_r32((c->r[16] + (uint32_t)28));
    gte_write_ctrl(6, c->r[13]);
    gte_write_ctrl(7, c->r[14]);
    c->r[2] = (uint32_t)32780u << 16;
    c->r[3] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-1936));
    c->r[2] = c->r[3] + (uint32_t)-1;
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)2);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)2048; if (_t) goto L_8002B220; }
    c->mem_w32((c->r[20] + (uint32_t)0), c->r[2]); goto L_8002B240;
  L_8002B220:;
    c->r[3] = c->r[3] & 255u;
    c->r[2] = c->r[0] + (uint32_t)7;
    { int _t = (c->r[3] != c->r[2]); c->r[2] = c->r[0] + (uint32_t)1024; if (_t) goto L_8002B23C; }
    c->mem_w32((c->r[20] + (uint32_t)0), c->r[2]);
    c->r[6] = c->r[0] + (uint32_t)-3; goto L_8002B244;
  L_8002B23C:;
    c->mem_w32((c->r[20] + (uint32_t)0), c->r[0]);
  L_8002B240:;
    c->r[6] = c->r[0] + (uint32_t)-10;
  L_8002B244:;
    c->r[4] = (uint32_t)32778u << 16;
    c->r[4] = c->r[4] + (uint32_t)-1304;
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x8002B258u;
    c->r[7] = c->r[5] + c->r[0]; func_80027768(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)60));
    c->r[20] = c->mem_r32((c->r[29] + (uint32_t)56));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)52));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)48));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)44));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)40));
    c->r[29] = c->r[29] + (uint32_t)64; return;
    return;
}

static void leaf_8002F514(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-40;
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[18]);
    c->r[18] = c->r[5] + c->r[0];
    c->r[3] = c->r[4] + (uint32_t)-1;
    c->r[2] = (uint32_t)(c->r[3] < (uint32_t)6);
    c->mem_w32((c->r[29] + (uint32_t)36), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[17]);
    { int _t = (c->r[2] == c->r[0]); c->mem_w32((c->r[29] + (uint32_t)24), c->r[16]); if (_t) goto L_8002F67C; }
    c->r[2] = (uint32_t)32769u << 16;
    c->r[2] = c->r[2] + (uint32_t)724;
    c->r[3] = c->r[3] << 2;
    c->r[3] = c->r[3] + c->r[2];
    c->r[2] = c->mem_r32((c->r[3] + (uint32_t)0));
    {  switch (c->r[2]) { case 0x8002F5F4u: goto L_8002F5F4; case 0x8002F558u: goto L_8002F558; case 0x8002F56Cu: goto L_8002F56C; case 0x8002F5B0u: goto L_8002F5B0; case 0x8002F638u: goto L_8002F638; case 0x8002F67Cu: goto L_8002F67C; default: rec_dispatch(c, c->r[2]); return; } }
  L_8002F558:;
    c->r[16] = c->r[0] + c->r[0];
    c->r[31] = 0x8002F564u;
    c->r[4] = c->r[18] + c->r[0]; func_80023528(c);
     goto L_8002F70C;
  L_8002F56C:;
    c->r[2] = (uint32_t)32782u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)32380));
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)7);
    { int _t = (c->r[2] != c->r[0]); c->r[16] = c->r[0] + c->r[0]; if (_t) goto L_8002F70C; }
    c->r[4] = c->r[0] + c->r[0];
    c->r[5] = c->r[0] + (uint32_t)6;
    c->r[31] = 0x8002F594u;
    c->r[6] = c->r[0] + (uint32_t)1; func_8007A980(c);
    c->r[16] = c->r[2] + c->r[0];
    { int _t = (c->r[16] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)-200; if (_t) goto L_8002F6D4; }
    c->mem_w16((c->r[16] + (uint32_t)50), (uint16_t)c->r[2]);
    c->r[4] = c->r[16] + c->r[0];
    c->r[5] = c->r[0] + (uint32_t)50; goto L_8002F704;
  L_8002F5B0:;
    c->r[2] = (uint32_t)32782u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)32380));
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)7);
    { int _t = (c->r[2] != c->r[0]); c->r[16] = c->r[0] + c->r[0]; if (_t) goto L_8002F70C; }
    c->r[4] = c->r[0] + c->r[0];
    c->r[5] = c->r[0] + (uint32_t)6;
    c->r[31] = 0x8002F5D8u;
    c->r[6] = c->r[0] + (uint32_t)1; func_8007A980(c);
    c->r[16] = c->r[2] + c->r[0];
    { int _t = (c->r[16] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)-200; if (_t) goto L_8002F6D4; }
    c->mem_w16((c->r[16] + (uint32_t)50), (uint16_t)c->r[2]);
    c->r[4] = c->r[16] + c->r[0];
    c->r[5] = c->r[0] + (uint32_t)51; goto L_8002F704;
  L_8002F5F4:;
    c->r[2] = (uint32_t)32782u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)32380));
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)7);
    { int _t = (c->r[2] != c->r[0]); c->r[16] = c->r[0] + c->r[0]; if (_t) goto L_8002F70C; }
    c->r[4] = c->r[0] + c->r[0];
    c->r[5] = c->r[0] + (uint32_t)6;
    c->r[31] = 0x8002F61Cu;
    c->r[6] = c->r[0] + (uint32_t)1; func_8007A980(c);
    c->r[16] = c->r[2] + c->r[0];
    { int _t = (c->r[16] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)-200; if (_t) goto L_8002F6D4; }
    c->mem_w16((c->r[16] + (uint32_t)50), (uint16_t)c->r[2]);
    c->r[4] = c->r[16] + c->r[0];
    c->r[5] = c->r[0] + (uint32_t)42; goto L_8002F704;
  L_8002F638:;
    c->r[2] = (uint32_t)32782u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)32380));
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)7);
    { int _t = (c->r[2] != c->r[0]); c->r[16] = c->r[0] + c->r[0]; if (_t) goto L_8002F70C; }
    c->r[4] = c->r[0] + c->r[0];
    c->r[5] = c->r[0] + (uint32_t)6;
    c->r[31] = 0x8002F660u;
    c->r[6] = c->r[0] + (uint32_t)1; func_8007A980(c);
    c->r[16] = c->r[2] + c->r[0];
    { int _t = (c->r[16] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)-200; if (_t) goto L_8002F6D4; }
    c->mem_w16((c->r[16] + (uint32_t)50), (uint16_t)c->r[2]);
    c->r[4] = c->r[16] + c->r[0];
    c->r[5] = c->r[0] + (uint32_t)49; goto L_8002F704;
  L_8002F67C:;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)352));
    c->r[3] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[29] + (uint32_t)16), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)354));
    c->r[3] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)356));
    c->r[2] = c->r[2] + (uint32_t)-200;
    c->mem_w16((c->r[29] + (uint32_t)18), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)32782u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)32380));
    c->r[17] = c->r[29] + (uint32_t)16;
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)7);
    { int _t = (c->r[2] != c->r[0]); c->mem_w16((c->r[29] + (uint32_t)20), (uint16_t)c->r[3]); if (_t) goto L_8002F6D4; }
    c->r[4] = c->r[0] + c->r[0];
    c->r[5] = c->r[0] + (uint32_t)6;
    c->r[31] = 0x8002F6C8u;
    c->r[6] = c->r[0] + (uint32_t)1; func_8007A980(c);
    c->r[16] = c->r[2] + c->r[0];
    { int _t = (c->r[16] != c->r[0]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_8002F6DC; }
  L_8002F6D4:;
    c->r[16] = c->r[0] + c->r[0]; goto L_8002F70C;
  L_8002F6DC:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[29] + (uint32_t)16));
    c->r[5] = c->r[0] + (uint32_t)54;
    c->mem_w16((c->r[4] + (uint32_t)44), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[17] + (uint32_t)2));
    c->mem_w16((c->r[4] + (uint32_t)46), (uint16_t)c->r[2]);
    c->r[3] = (uint32_t)c->mem_r16((c->r[17] + (uint32_t)4));
    c->r[2] = c->r[0] + (uint32_t)-200;
    c->mem_w16((c->r[4] + (uint32_t)50), (uint16_t)c->r[2]);
    c->mem_w16((c->r[4] + (uint32_t)48), (uint16_t)c->r[3]);
  L_8002F704:;
    c->r[31] = 0x8002F70Cu;
     func_80028E10(c);
  L_8002F70C:;
    { int _t = (c->r[18] == c->r[0]);  if (_t) goto L_8002F720; }
    { int _t = (c->r[16] == c->r[0]);  if (_t) goto L_8002F720; }
    c->mem_w32((c->r[16] + (uint32_t)16), c->r[18]);
  L_8002F720:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)36));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[29] = c->r[29] + (uint32_t)40; return;
    return;
}

static void leaf_800317CC(Core* c) {
    c->r[5] = (uint32_t)8064u << 16;
    c->r[3] = c->r[5] + (uint32_t)128;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[7] = c->r[2] + (uint32_t)132;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[6] = c->r[2] + (uint32_t)140;
    gte_op(c, 0x4A180001u);
    c->r[12] = gte_read_ctrl(31);
    c->mem_w32((c->r[3] + (uint32_t)0), c->r[12]);
    c->r[2] = c->mem_r32((c->r[5] + (uint32_t)128));
    { int _t = ((int32_t)c->r[2] >= 0);  if (_t) goto L_80031814; }
  L_8003180C:;
    c->r[2] = c->r[0] + (uint32_t)1; return;
  L_80031814:;
    c->mem_w32((c->r[3] + (uint32_t)0), gte_read_data(19));
    c->r[3] = c->mem_r32((c->r[5] + (uint32_t)128));
    { int _t = ((int32_t)c->r[3] <= 0); c->r[3] = (uint32_t)((int32_t)c->r[3] >> 2); if (_t) goto L_8003180C; }
    c->r[2] = c->r[4] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[3] = c->r[3] + c->r[2];
    c->mem_w32((c->r[5] + (uint32_t)128), c->r[3]);
    c->r[3] = (uint32_t)((int32_t)c->r[3] < 4);
    { int _t = (c->r[3] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)4; if (_t) goto L_80031848; }
    c->mem_w32((c->r[5] + (uint32_t)128), c->r[2]);
  L_80031848:;
    c->r[2] = c->mem_r32((c->r[5] + (uint32_t)128));
    c->r[3] = (uint32_t)((int32_t)c->r[2] >> 10);
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> (c->r[3] & 31));
    c->r[3] = c->r[3] << 9;
    c->r[2] = c->r[2] + c->r[3];
    c->mem_w32((c->r[5] + (uint32_t)128), c->r[2]);
    c->r[2] = c->r[2] + (uint32_t)-4;
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)2044);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)-1; if (_t) goto L_80031878; }
    c->mem_w32((c->r[5] + (uint32_t)128), c->r[2]);
  L_80031878:;
    c->r[2] = c->mem_r32((c->r[5] + (uint32_t)128));
    { int _t = ((int32_t)c->r[2] < 0);  if (_t) goto L_80031898; }
    c->mem_w32((c->r[6] + (uint32_t)0), gte_read_data(14));
    c->mem_w32((c->r[7] + (uint32_t)0), gte_read_data(24));
    c->r[2] = c->r[0] + c->r[0]; return;
  L_80031898:;
    c->r[2] = c->r[0] + (uint32_t)1; return;
    return;
}

static void leaf_800329E0(Core* c) {
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = c->r[2] + (uint32_t)248;
    c->r[12] = c->mem_r32((c->r[2] + (uint32_t)0));
    c->r[13] = c->mem_r32((c->r[2] + (uint32_t)4));
    gte_write_ctrl(0, c->r[12]);
    gte_write_ctrl(1, c->r[13]);
    c->r[12] = c->mem_r32((c->r[2] + (uint32_t)8));
    c->r[13] = c->mem_r32((c->r[2] + (uint32_t)12));
    c->r[14] = c->mem_r32((c->r[2] + (uint32_t)16));
    gte_write_ctrl(2, c->r[12]);
    gte_write_ctrl(3, c->r[13]);
    gte_write_ctrl(4, c->r[14]);
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = c->r[2] + (uint32_t)248;
    c->r[12] = c->mem_r32((c->r[2] + (uint32_t)20));
    c->r[13] = c->mem_r32((c->r[2] + (uint32_t)24));
    gte_write_ctrl(5, c->r[12]);
    c->r[14] = c->mem_r32((c->r[2] + (uint32_t)28));
    gte_write_ctrl(6, c->r[13]);
    gte_write_ctrl(7, c->r[14]);
    gte_write_ctrl(27, c->r[4]);
    c->r[10] = c->r[0] + (uint32_t)0;
    gte_write_ctrl(28, c->r[10]);
     return;
    return;
}

static void leaf_80033AFC(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-40;
    c->r[2] = c->r[0] + (uint32_t)8192;
    { int _t = (c->r[4] == c->r[2]); c->mem_w32((c->r[29] + (uint32_t)32), c->r[31]); if (_t) goto L_80033B3C; }
    c->r[2] = (uint32_t)((int32_t)c->r[4] < 8193);
    { int _t = (c->r[2] != c->r[0]); c->r[3] = c->r[0] + (uint32_t)118; if (_t) goto L_80033B40; }
    c->r[2] = c->r[0] + (uint32_t)16384;
    { int _t = (c->r[4] == c->r[2]); c->r[2] = c->r[0] | 32768u; if (_t) goto L_80033B34; }
    { int _t = (c->r[4] != c->r[2]); c->r[2] = c->r[0] + (uint32_t)31934; if (_t) goto L_80033B44; }
    c->r[3] = c->r[0] + (uint32_t)119; goto L_80033B44;
  L_80033B34:;
    c->r[3] = c->r[0] + (uint32_t)117; goto L_80033B40;
  L_80033B3C:;
    c->r[3] = c->r[0] + (uint32_t)116;
  L_80033B40:;
    c->r[2] = c->r[0] + (uint32_t)31934;
  L_80033B44:;
    c->mem_w16((c->r[29] + (uint32_t)18), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)32769u << 16;
    c->r[2] = c->r[2] + (uint32_t)29492;
    c->r[3] = c->r[3] << 2;
    c->r[3] = c->r[3] + c->r[2];
    c->r[2] = (uint32_t)32783u << 16;
    c->r[4] = c->r[29] + (uint32_t)24;
    c->mem_w8((c->r[29] + (uint32_t)17), (uint8_t)c->r[7]);
    c->mem_w8((c->r[29] + (uint32_t)16), (uint8_t)c->r[0]);
    c->mem_w16((c->r[29] + (uint32_t)24), (uint16_t)c->r[5]);
    c->mem_w16((c->r[29] + (uint32_t)26), (uint16_t)c->r[6]);
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[0]);
    c->r[5] = c->mem_r32((c->r[3] + (uint32_t)0));
    c->r[6] = c->mem_r32((c->r[2] + (uint32_t)-12456));
    c->r[31] = 0x80033B84u;
    c->r[7] = c->r[29] + (uint32_t)16; func_8007E6DC(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[29] = c->r[29] + (uint32_t)40; return;
    return;
}

static void leaf_8003A290(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[5] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[31]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)62));
    c->r[6] = c->r[0] + (uint32_t)1;
    { int _t = (c->r[3] == c->r[6]); c->r[17] = c->r[4] + c->r[0]; if (_t) goto L_8003A338; }
    c->r[2] = (uint32_t)((int32_t)c->r[3] < 2);
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_8003A2D0; }
    { int _t = (c->r[3] == c->r[0]); c->r[4] = c->r[17] + c->r[0]; if (_t) goto L_8003A2E4; }
     goto L_8003A3D4;
  L_8003A2D0:;
    c->r[2] = c->r[0] + (uint32_t)2;
    { int _t = (c->r[3] == c->r[2]);  if (_t) goto L_8003A3B4; }
     goto L_8003A3D4;
  L_8003A2E4:;
    c->r[5] = c->r[16] + c->r[0];
    c->r[31] = 0x8003A2F0u;
    c->mem_w8((c->r[16] + (uint32_t)62), (uint8_t)c->r[6]); func_8003A1E4(c);
    c->r[3] = (uint32_t)10922u << 16;
    c->r[3] = c->r[3] | 43691u;
    c->r[2] = c->r[2] << 4;
    { int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[3]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 31);
    c->mem_w16((c->r[16] + (uint32_t)16), (uint16_t)c->r[0]);
    c->r[7] = c->hi;
    c->r[3] = (uint32_t)((int32_t)c->r[7] >> 2);
    c->r[3] = c->r[3] - c->r[2];
    c->r[31] = 0x8003A31Cu;
    c->mem_w16((c->r[16] + (uint32_t)22), (uint16_t)c->r[3]); func_8009A450(c);
    c->r[2] = c->r[2] & 15u;
    c->r[2] = c->r[2] + (uint32_t)-192;
    c->r[31] = 0x8003A32Cu;
    c->mem_w16((c->r[16] + (uint32_t)18), (uint16_t)c->r[2]); func_8009A450(c);
    c->r[2] = c->r[2] & 63u;
    c->r[2] = c->r[2] << 6;
    c->mem_w16((c->r[16] + (uint32_t)8), (uint16_t)c->r[2]);
  L_8003A338:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)16));
    c->r[3] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)22));
    c->r[4] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)18));
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 20);
    c->r[4] = c->r[4] << 16;
    c->mem_w16((c->r[16] + (uint32_t)0), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)16));
    c->r[4] = (uint32_t)((int32_t)c->r[4] >> 20);
    c->r[2] = c->r[2] + c->r[3];
    c->mem_w16((c->r[16] + (uint32_t)16), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)2));
    c->r[3] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)18));
    c->r[2] = c->r[2] + c->r[4];
    c->mem_w16((c->r[16] + (uint32_t)2), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)8));
    c->r[3] = c->r[3] + (uint32_t)16;
    c->mem_w16((c->r[16] + (uint32_t)18), (uint16_t)c->r[3]);
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)2));
    c->r[2] = c->r[2] + (uint32_t)128;
    { int _t = ((int32_t)c->r[3] < 0); c->mem_w16((c->r[16] + (uint32_t)8), (uint16_t)c->r[2]); if (_t) goto L_8003A3D4; }
    c->r[4] = c->r[17] + c->r[0];
    c->r[31] = 0x8003A39Cu;
    c->r[5] = c->r[16] + c->r[0]; func_8003A1E4(c);
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)62));
    c->mem_w16((c->r[16] + (uint32_t)0), (uint16_t)c->r[2]);
    c->mem_w16((c->r[16] + (uint32_t)8), (uint16_t)c->r[0]);
    c->r[3] = c->r[3] + (uint32_t)1;
    c->mem_w8((c->r[16] + (uint32_t)62), (uint8_t)c->r[3]); goto L_8003A3D4;
  L_8003A3B4:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)63));
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_8003A3D4; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)5));
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w8((c->r[17] + (uint32_t)5), (uint8_t)c->r[2]);
  L_8003A3D4:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

static void leaf_8003A3E8(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[5] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[31]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)62));
    { int _t = (c->r[3] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_8003A418; }
    { int _t = (c->r[3] == c->r[2]);  if (_t) goto L_8003A438; }
     goto L_8003A460;
  L_8003A418:;
    c->r[5] = c->r[16] + c->r[0];
    c->r[31] = 0x8003A424u;
    c->mem_w8((c->r[16] + (uint32_t)62), (uint8_t)c->r[2]); func_8003A1E4(c);
    c->r[31] = 0x8003A42Cu;
    c->mem_w16((c->r[16] + (uint32_t)0), (uint16_t)c->r[2]); func_8009A450(c);
    c->r[2] = c->r[2] & 31u;
    c->r[2] = c->r[2] << 7;
    c->mem_w16((c->r[16] + (uint32_t)18), (uint16_t)c->r[2]);
  L_8003A438:;
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)18));
    c->r[31] = 0x8003A444u;
     func_80083F50(c);
    c->r[3] = c->r[2] << 1;
    c->r[3] = c->r[3] + c->r[2];
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)18));
    c->r[3] = (uint32_t)((int32_t)c->r[3] >> 12);
    c->mem_w16((c->r[16] + (uint32_t)2), (uint16_t)c->r[3]);
    c->r[2] = c->r[2] + (uint32_t)128;
    c->mem_w16((c->r[16] + (uint32_t)18), (uint16_t)c->r[2]);
  L_8003A460:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_8003A470(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[17] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[5] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[31]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)62));
    c->r[4] = c->r[0] + (uint32_t)1;
    { int _t = (c->r[3] == c->r[4]); c->r[2] = (uint32_t)((int32_t)c->r[3] < 2); if (_t) goto L_8003A4D4; }
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_8003A4B0; }
    { int _t = (c->r[3] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)-384; if (_t) goto L_8003A4CC; }
     goto L_8003A5D0;
  L_8003A4B0:;
    c->r[4] = c->r[0] + (uint32_t)2;
    { int _t = (c->r[3] == c->r[4]); c->r[2] = c->r[0] + (uint32_t)3; if (_t) goto L_8003A550; }
    { int _t = (c->r[3] == c->r[2]);  if (_t) goto L_8003A5BC; }
     goto L_8003A5D0;
  L_8003A4CC:;
    c->mem_w8((c->r[16] + (uint32_t)62), (uint8_t)c->r[4]);
    c->mem_w16((c->r[16] + (uint32_t)18), (uint16_t)c->r[2]);
  L_8003A4D4:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)62));
    c->r[2] = c->r[2] + (uint32_t)1;
    c->r[31] = 0x8003A4E8u;
    c->mem_w8((c->r[16] + (uint32_t)62), (uint8_t)c->r[2]); func_8009A450(c);
    c->r[2] = c->r[2] & 31u;
    c->r[2] = c->r[2] + (uint32_t)-192;
    c->r[3] = c->r[0] + (uint32_t)6;
    c->r[4] = c->r[17] + c->r[0];
    c->mem_w16((c->r[16] + (uint32_t)18), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)0));
    c->r[5] = c->r[16] + c->r[0];
    c->mem_w16((c->r[16] + (uint32_t)22), (uint16_t)c->r[3]);
    c->r[2] = c->r[2] << 4;
    c->r[31] = 0x8003A514u;
    c->mem_w16((c->r[16] + (uint32_t)16), (uint16_t)c->r[2]); func_8003A1E4(c);
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)22));
    c->r[2] = c->r[2] << 4;
    cpu_div(c, c->r[2], c->r[3]);
    { int _t = (c->r[3] != c->r[0]);  if (_t) goto L_8003A52C; }
    rec_break(c, 7168u);
  L_8003A52C:;
    c->r[1] = c->r[0] + (uint32_t)-1;
    { int _t = (c->r[3] != c->r[1]); c->r[1] = (uint32_t)32768u << 16; if (_t) goto L_8003A544; }
    { int _t = (c->r[2] != c->r[1]);  if (_t) goto L_8003A544; }
    rec_break(c, 6144u);
  L_8003A544:;
    c->r[2] = c->lo;
    c->mem_w16((c->r[16] + (uint32_t)56), (uint16_t)c->r[2]); goto L_8003A5D0;
  L_8003A550:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)18));
    c->r[3] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)2));
    c->r[4] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)56));
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 20);
    c->r[3] = c->r[3] + c->r[2];
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)16));
    c->mem_w16((c->r[16] + (uint32_t)2), (uint16_t)c->r[3]);
    c->r[3] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)18));
    c->r[2] = c->r[2] + c->r[4];
    c->mem_w16((c->r[16] + (uint32_t)16), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 20);
    c->mem_w16((c->r[16] + (uint32_t)0), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)8));
    c->r[3] = c->r[3] + (uint32_t)32;
    c->mem_w16((c->r[16] + (uint32_t)18), (uint16_t)c->r[3]);
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)2));
    c->r[2] = c->r[2] + (uint32_t)12;
    c->r[3] = (uint32_t)((int32_t)c->r[3] < 240);
    { int _t = (c->r[3] != c->r[0]); c->mem_w16((c->r[16] + (uint32_t)8), (uint16_t)c->r[2]); if (_t) goto L_8003A5D0; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)62));
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w8((c->r[16] + (uint32_t)62), (uint8_t)c->r[2]); goto L_8003A5D0;
  L_8003A5BC:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)63));
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_8003A5D0; }
    c->mem_w8((c->r[17] + (uint32_t)4), (uint8_t)c->r[4]);
  L_8003A5D0:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

static void leaf_8003A5E4(Core* c) {
    c->r[3] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)62));
    c->r[2] = (uint32_t)(c->r[3] < (uint32_t)5);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)32769u << 16; if (_t) goto L_8003A788; }
    c->r[2] = c->r[2] + (uint32_t)18980;
    c->r[3] = c->r[3] << 2;
    c->r[3] = c->r[3] + c->r[2];
    c->r[2] = c->mem_r32((c->r[3] + (uint32_t)0));
    {  switch (c->r[2]) { case 0x8003A614u: goto L_8003A614; case 0x8003A678u: goto L_8003A678; case 0x8003A6D4u: goto L_8003A6D4; case 0x8003A724u: goto L_8003A724; case 0x8003A774u: goto L_8003A774; default: rec_dispatch(c, c->r[2]); return; } }
  L_8003A614:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)62));
    c->r[3] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)0));
    c->r[2] = c->r[2] + (uint32_t)1;
    c->r[3] = c->r[3] << 16;
    c->r[4] = (uint32_t)((int32_t)c->r[3] >> 16);
    { int _t = ((int32_t)c->r[4] >= 0); c->mem_w8((c->r[5] + (uint32_t)62), (uint8_t)c->r[2]); if (_t) goto L_8003A650; }
    c->r[2] = (uint32_t)21845u << 16;
    c->r[2] = c->r[2] | 21846u;
    { int64_t _p = (int64_t)(int32_t)c->r[4] * (int64_t)(int32_t)c->r[2]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[2] = (uint32_t)((int32_t)c->r[3] >> 31);
    c->r[6] = c->hi;
    c->r[2] = c->r[6] - c->r[2];
    c->r[2] = c->r[2] + (uint32_t)-5; goto L_8003A66C;
  L_8003A650:;
    c->r[2] = (uint32_t)21845u << 16;
    c->r[2] = c->r[2] | 21846u;
    { int64_t _p = (int64_t)(int32_t)c->r[4] * (int64_t)(int32_t)c->r[2]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[2] = (uint32_t)((int32_t)c->r[3] >> 31);
    c->r[6] = c->hi;
    c->r[2] = c->r[6] - c->r[2];
    c->r[2] = c->r[2] + (uint32_t)5;
  L_8003A66C:;
    c->mem_w16((c->r[5] + (uint32_t)16), (uint16_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)-256;
    c->mem_w16((c->r[5] + (uint32_t)18), (uint16_t)c->r[2]);
  L_8003A678:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)0));
    c->r[3] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)16));
    c->r[4] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)18));
    c->r[2] = c->r[2] + c->r[3];
    c->r[4] = c->r[4] << 16;
    c->r[3] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)2));
    c->r[4] = (uint32_t)((int32_t)c->r[4] >> 20);
    c->mem_w16((c->r[5] + (uint32_t)0), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)18));
    c->r[3] = c->r[3] + c->r[4];
    c->r[2] = c->r[2] + (uint32_t)32;
    c->mem_w16((c->r[5] + (uint32_t)18), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 257);
    { int _t = (c->r[2] != c->r[0]); c->mem_w16((c->r[5] + (uint32_t)2), (uint16_t)c->r[3]); if (_t) goto L_8003A788; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)62));
    c->r[3] = c->r[0] + (uint32_t)-192;
  L_8003A6C4:;
    c->mem_w16((c->r[5] + (uint32_t)18), (uint16_t)c->r[3]);
    c->r[2] = c->r[2] + (uint32_t)1;
  L_8003A6CC:;
    c->mem_w8((c->r[5] + (uint32_t)62), (uint8_t)c->r[2]); return;
  L_8003A6D4:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)0));
    c->r[3] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)16));
    c->r[4] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)18));
    c->r[2] = c->r[2] + c->r[3];
    c->r[4] = c->r[4] << 16;
    c->r[3] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)2));
    c->r[4] = (uint32_t)((int32_t)c->r[4] >> 20);
    c->mem_w16((c->r[5] + (uint32_t)0), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)18));
    c->r[3] = c->r[3] + c->r[4];
    c->r[2] = c->r[2] + (uint32_t)24;
    c->mem_w16((c->r[5] + (uint32_t)18), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 193);
    { int _t = (c->r[2] != c->r[0]); c->mem_w16((c->r[5] + (uint32_t)2), (uint16_t)c->r[3]); if (_t) goto L_8003A788; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)62));
    c->r[3] = c->r[0] + (uint32_t)-128; goto L_8003A6C4;
  L_8003A724:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)0));
    c->r[3] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)16));
    c->r[4] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)18));
    c->r[2] = c->r[2] + c->r[3];
    c->r[4] = c->r[4] << 16;
    c->r[3] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)2));
    c->r[4] = (uint32_t)((int32_t)c->r[4] >> 20);
    c->mem_w16((c->r[5] + (uint32_t)0), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)18));
    c->r[3] = c->r[3] + c->r[4];
    c->r[2] = c->r[2] + (uint32_t)16;
    c->mem_w16((c->r[5] + (uint32_t)18), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 129);
    { int _t = (c->r[2] != c->r[0]); c->mem_w16((c->r[5] + (uint32_t)2), (uint16_t)c->r[3]); if (_t) goto L_8003A788; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)62));
    c->r[2] = c->r[2] + (uint32_t)1; goto L_8003A6CC;
  L_8003A774:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)63));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)2; if (_t) goto L_8003A788; }
    c->mem_w8((c->r[4] + (uint32_t)4), (uint8_t)c->r[2]);
  L_8003A788:;
     return;
    return;
}

static void leaf_8003A790(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
    c->r[18] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)5));
    c->r[2] = c->r[0] + (uint32_t)1;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = (uint32_t)((int32_t)c->r[3] < 2); if (_t) goto L_8003A864; }
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_8003A7D0; }
    { int _t = (c->r[3] == c->r[0]);  if (_t) goto L_8003A7EC; }
     goto L_8003A988;
  L_8003A7D0:;
    c->r[2] = c->r[0] + (uint32_t)2;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = c->r[0] + (uint32_t)3; if (_t) goto L_8003A8A4; }
    { int _t = (c->r[3] == c->r[2]);  if (_t) goto L_8003A950; }
     goto L_8003A988;
  L_8003A7EC:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)8));
    { int _t = (c->r[2] == c->r[0]); c->r[16] = c->r[0] + c->r[0]; if (_t) goto L_8003A824; }
    c->r[17] = c->r[18] + c->r[0];
    c->r[4] = c->r[18] + c->r[0];
  L_8003A804:;
    c->r[5] = c->mem_r32((c->r[17] + (uint32_t)192));
    c->r[31] = 0x8003A810u;
    c->r[17] = c->r[17] + (uint32_t)4; func_8003A290(c);
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)8));
    c->r[16] = c->r[16] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[16] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[18] + c->r[0]; if (_t) goto L_8003A804; }
  L_8003A824:;
    c->r[4] = (uint32_t)32784u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[18] + (uint32_t)54));
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)1016));
    c->r[2] = c->r[2] + (uint32_t)-16;
    c->mem_w16((c->r[18] + (uint32_t)54), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[3] = c->r[3] + (uint32_t)8;
    c->r[2] = (uint32_t)((int32_t)c->r[2] < (int32_t)c->r[3]);
    c->r[3] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)1016));
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[3] + (uint32_t)8; if (_t) goto L_8003A988; }
    c->mem_w16((c->r[18] + (uint32_t)54), (uint16_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)1;
    c->mem_w8((c->r[18] + (uint32_t)94), (uint8_t)c->r[2]); goto L_8003A988;
  L_8003A864:;
    c->r[16] = c->r[0] + c->r[0];
    c->r[3] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)8));
    c->r[2] = c->r[0] + (uint32_t)2;
    c->mem_w8((c->r[18] + (uint32_t)5), (uint8_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)60;
    { int _t = (c->r[3] == c->r[0]); c->mem_w16((c->r[18] + (uint32_t)64), (uint16_t)c->r[2]); if (_t) goto L_8003A8A4; }
    c->r[3] = c->r[18] + c->r[0];
  L_8003A884:;
    c->r[2] = c->mem_r32((c->r[3] + (uint32_t)192));
    c->mem_w8((c->r[2] + (uint32_t)62), (uint8_t)c->r[0]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)8));
    c->r[16] = c->r[16] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[16] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]); c->r[3] = c->r[3] + (uint32_t)4; if (_t) goto L_8003A884; }
  L_8003A8A4:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[18] + (uint32_t)64));
    c->r[3] = c->r[0] + (uint32_t)-1;
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->mem_w16((c->r[18] + (uint32_t)64), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    { int _t = (c->r[2] != c->r[3]);  if (_t) goto L_8003A910; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)8));
    { int _t = (c->r[2] == c->r[0]); c->r[16] = c->r[0] + c->r[0]; if (_t) goto L_8003A8F8; }
    c->r[3] = c->r[18] + c->r[0];
  L_8003A8D8:;
    c->r[2] = c->mem_r32((c->r[3] + (uint32_t)192));
    c->mem_w8((c->r[2] + (uint32_t)62), (uint8_t)c->r[0]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)8));
    c->r[16] = c->r[16] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[16] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]); c->r[3] = c->r[3] + (uint32_t)4; if (_t) goto L_8003A8D8; }
  L_8003A8F8:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)5));
    c->r[3] = c->r[0] + (uint32_t)2;
    c->mem_w8((c->r[18] + (uint32_t)94), (uint8_t)c->r[3]);
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w8((c->r[18] + (uint32_t)5), (uint8_t)c->r[2]); goto L_8003A988;
  L_8003A910:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)8));
    { int _t = (c->r[2] == c->r[0]); c->r[16] = c->r[0] + c->r[0]; if (_t) goto L_8003A988; }
    c->r[17] = c->r[18] + c->r[0];
    c->r[4] = c->r[18] + c->r[0];
  L_8003A928:;
    c->r[5] = c->mem_r32((c->r[17] + (uint32_t)192));
    c->r[31] = 0x8003A934u;
    c->r[17] = c->r[17] + (uint32_t)4; func_8003A3E8(c);
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)8));
    c->r[16] = c->r[16] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[16] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[18] + c->r[0]; if (_t) goto L_8003A928; }
     goto L_8003A988;
  L_8003A950:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)8));
    { int _t = (c->r[2] == c->r[0]); c->r[16] = c->r[0] + c->r[0]; if (_t) goto L_8003A988; }
    c->r[17] = c->r[18] + c->r[0];
    c->r[4] = c->r[18] + c->r[0];
  L_8003A968:;
    c->r[5] = c->mem_r32((c->r[17] + (uint32_t)192));
    c->r[31] = 0x8003A974u;
    c->r[17] = c->r[17] + (uint32_t)4; func_8003A5E4(c);
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)8));
    c->r[16] = c->r[16] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[16] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[18] + c->r[0]; if (_t) goto L_8003A968; }
  L_8003A988:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

static void leaf_8003A9A0(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
    c->r[18] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)5));
    c->r[2] = (uint32_t)(c->r[3] < (uint32_t)6);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)32769u << 16; if (_t) goto L_8003ABCC; }
    c->r[2] = c->r[2] + (uint32_t)19004;
    c->r[3] = c->r[3] << 2;
    c->r[3] = c->r[3] + c->r[2];
    c->r[2] = c->mem_r32((c->r[3] + (uint32_t)0));
    {  switch (c->r[2]) { case 0x8003A9E8u: goto L_8003A9E8; case 0x8003AA60u: goto L_8003AA60; case 0x8003AAA4u: goto L_8003AAA4; case 0x8003AB70u: goto L_8003AB70; case 0x8003AB80u: goto L_8003AB80; default: rec_dispatch(c, c->r[2]); return; } }
  L_8003A9E8:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)8));
    { int _t = (c->r[2] == c->r[0]); c->r[16] = c->r[0] + c->r[0]; if (_t) goto L_8003AA20; }
    c->r[17] = c->r[18] + c->r[0];
    c->r[4] = c->r[18] + c->r[0];
  L_8003AA00:;
    c->r[5] = c->mem_r32((c->r[17] + (uint32_t)192));
    c->r[31] = 0x8003AA0Cu;
    c->r[17] = c->r[17] + (uint32_t)4; func_8003A290(c);
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)8));
    c->r[16] = c->r[16] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[16] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[18] + c->r[0]; if (_t) goto L_8003AA00; }
  L_8003AA20:;
    c->r[4] = (uint32_t)32784u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[18] + (uint32_t)54));
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)1016));
    c->r[2] = c->r[2] + (uint32_t)-16;
    c->mem_w16((c->r[18] + (uint32_t)54), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[3] = c->r[3] + (uint32_t)8;
    c->r[2] = (uint32_t)((int32_t)c->r[2] < (int32_t)c->r[3]);
    c->r[3] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)1016));
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[3] + (uint32_t)8; if (_t) goto L_8003ABCC; }
    c->mem_w16((c->r[18] + (uint32_t)54), (uint16_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)1;
    c->mem_w8((c->r[18] + (uint32_t)94), (uint8_t)c->r[2]); goto L_8003ABCC;
  L_8003AA60:;
    c->r[16] = c->r[0] + c->r[0];
    c->r[2] = c->r[0] + (uint32_t)40;
    c->mem_w16((c->r[18] + (uint32_t)64), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)5));
    c->r[3] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)8));
    c->r[2] = c->r[2] + (uint32_t)1;
    { int _t = (c->r[3] == c->r[0]); c->mem_w8((c->r[18] + (uint32_t)5), (uint8_t)c->r[2]); if (_t) goto L_8003AAA4; }
    c->r[3] = c->r[18] + c->r[0];
  L_8003AA84:;
    c->r[2] = c->mem_r32((c->r[3] + (uint32_t)192));
    c->mem_w8((c->r[2] + (uint32_t)62), (uint8_t)c->r[0]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)8));
    c->r[16] = c->r[16] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[16] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]); c->r[3] = c->r[3] + (uint32_t)4; if (_t) goto L_8003AA84; }
  L_8003AAA4:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[18] + (uint32_t)64));
    c->r[3] = c->r[0] + (uint32_t)-1;
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->mem_w16((c->r[18] + (uint32_t)64), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    { int _t = (c->r[2] != c->r[3]);  if (_t) goto L_8003AB30; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)8));
    { int _t = (c->r[2] == c->r[0]); c->r[16] = c->r[0] + c->r[0]; if (_t) goto L_8003AAF8; }
    c->r[3] = c->r[18] + c->r[0];
  L_8003AAD8:;
    c->r[2] = c->mem_r32((c->r[3] + (uint32_t)192));
    c->mem_w8((c->r[2] + (uint32_t)62), (uint8_t)c->r[0]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)8));
    c->r[16] = c->r[16] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[16] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]); c->r[3] = c->r[3] + (uint32_t)4; if (_t) goto L_8003AAD8; }
  L_8003AAF8:;
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[18] + (uint32_t)96));
    c->r[31] = 0x8003AB04u;
    c->r[5] = c->r[0] + (uint32_t)2; func_80040AA4(c);
    c->r[2] = (uint32_t)32783u << 16;
    c->r[2] = c->r[2] + (uint32_t)-12200;
    c->r[3] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)20));
    c->r[3] = c->r[3] + (uint32_t)1;
    c->mem_w8((c->r[2] + (uint32_t)20), (uint8_t)c->r[3]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)5));
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w8((c->r[18] + (uint32_t)5), (uint8_t)c->r[2]); goto L_8003ABCC;
  L_8003AB30:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)8));
    { int _t = (c->r[2] == c->r[0]); c->r[16] = c->r[0] + c->r[0]; if (_t) goto L_8003ABCC; }
    c->r[17] = c->r[18] + c->r[0];
    c->r[4] = c->r[18] + c->r[0];
  L_8003AB48:;
    c->r[5] = c->mem_r32((c->r[17] + (uint32_t)192));
    c->r[31] = 0x8003AB54u;
    c->r[17] = c->r[17] + (uint32_t)4; func_8003A3E8(c);
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)8));
    c->r[16] = c->r[16] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[16] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[18] + c->r[0]; if (_t) goto L_8003AB48; }
     goto L_8003ABCC;
  L_8003AB70:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)5));
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w8((c->r[18] + (uint32_t)5), (uint8_t)c->r[2]);
  L_8003AB80:;
    c->r[16] = c->r[0] + c->r[0];
    c->r[3] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)8));
    c->r[2] = c->r[0] + (uint32_t)2;
    { int _t = (c->r[3] == c->r[0]); c->mem_w8((c->r[18] + (uint32_t)94), (uint8_t)c->r[2]); if (_t) goto L_8003ABBC; }
    c->r[17] = c->r[18] + c->r[0];
    c->r[4] = c->r[18] + c->r[0];
  L_8003AB9C:;
    c->r[5] = c->mem_r32((c->r[17] + (uint32_t)192));
    c->r[31] = 0x8003ABA8u;
    c->r[17] = c->r[17] + (uint32_t)4; func_8003A470(c);
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)8));
    c->r[16] = c->r[16] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[16] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[18] + c->r[0]; if (_t) goto L_8003AB9C; }
  L_8003ABBC:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[18] + (uint32_t)54));
    c->r[2] = c->r[2] + (uint32_t)-4;
    c->mem_w16((c->r[18] + (uint32_t)54), (uint16_t)c->r[2]);
  L_8003ABCC:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

static void leaf_8003ABE4(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
    c->r[18] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)5));
    c->r[4] = c->r[0] + (uint32_t)1;
    { int _t = (c->r[3] == c->r[4]); c->r[2] = (uint32_t)((int32_t)c->r[3] < 2); if (_t) goto L_8003AC3C; }
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)2; if (_t) goto L_8003AC24; }
    { int _t = (c->r[3] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)40; if (_t) goto L_8003AC34; }
     goto L_8003AD30;
  L_8003AC24:;
    { int _t = (c->r[3] == c->r[2]);  if (_t) goto L_8003ACE8; }
     goto L_8003AD30;
  L_8003AC34:;
    c->mem_w8((c->r[18] + (uint32_t)5), (uint8_t)c->r[4]);
    c->mem_w16((c->r[18] + (uint32_t)64), (uint16_t)c->r[2]);
  L_8003AC3C:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[18] + (uint32_t)64));
    c->r[3] = c->r[0] + (uint32_t)-1;
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->mem_w16((c->r[18] + (uint32_t)64), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    { int _t = (c->r[2] != c->r[3]);  if (_t) goto L_8003ACA8; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)8));
    { int _t = (c->r[2] == c->r[0]); c->r[16] = c->r[0] + c->r[0]; if (_t) goto L_8003AC90; }
    c->r[3] = c->r[18] + c->r[0];
  L_8003AC70:;
    c->r[2] = c->mem_r32((c->r[3] + (uint32_t)192));
    c->mem_w8((c->r[2] + (uint32_t)62), (uint8_t)c->r[0]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)8));
    c->r[16] = c->r[16] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[16] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]); c->r[3] = c->r[3] + (uint32_t)4; if (_t) goto L_8003AC70; }
  L_8003AC90:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)5));
    c->r[3] = c->r[0] + (uint32_t)2;
    c->mem_w8((c->r[18] + (uint32_t)94), (uint8_t)c->r[3]);
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w8((c->r[18] + (uint32_t)5), (uint8_t)c->r[2]); goto L_8003AD30;
  L_8003ACA8:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)8));
    { int _t = (c->r[2] == c->r[0]); c->r[16] = c->r[0] + c->r[0]; if (_t) goto L_8003AD30; }
    c->r[17] = c->r[18] + c->r[0];
    c->r[4] = c->r[18] + c->r[0];
  L_8003ACC0:;
    c->r[5] = c->mem_r32((c->r[17] + (uint32_t)192));
    c->r[31] = 0x8003ACCCu;
    c->r[17] = c->r[17] + (uint32_t)4; func_8003A3E8(c);
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)8));
    c->r[16] = c->r[16] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[16] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[18] + c->r[0]; if (_t) goto L_8003ACC0; }
     goto L_8003AD30;
  L_8003ACE8:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)8));
    { int _t = (c->r[2] == c->r[0]); c->r[16] = c->r[0] + c->r[0]; if (_t) goto L_8003AD20; }
    c->r[17] = c->r[18] + c->r[0];
    c->r[4] = c->r[18] + c->r[0];
  L_8003AD00:;
    c->r[5] = c->mem_r32((c->r[17] + (uint32_t)192));
    c->r[31] = 0x8003AD0Cu;
    c->r[17] = c->r[17] + (uint32_t)4; func_8003A470(c);
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)8));
    c->r[16] = c->r[16] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[16] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[18] + c->r[0]; if (_t) goto L_8003AD00; }
  L_8003AD20:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[18] + (uint32_t)54));
    c->r[2] = c->r[2] + (uint32_t)-4;
    c->mem_w16((c->r[18] + (uint32_t)54), (uint16_t)c->r[2]);
  L_8003AD30:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

static void leaf_8003B588(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->r[2] = (uint32_t)32782u << 16;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[2] + (uint32_t)32384;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)13));
    c->r[2] = c->r[3] & 208u;
    { int _t = (c->r[2] == c->r[0]); c->r[3] = c->r[3] | 2u; if (_t) goto L_8003B698; }
    c->r[2] = c->r[3] & 32u;
    { int _t = (c->r[2] != c->r[0]); c->mem_w8((c->r[16] + (uint32_t)13), (uint8_t)c->r[3]); if (_t) goto L_8003B69C; }
    c->r[2] = c->r[3] & 16u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_8003B64C; }
    c->r[3] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)583));
    c->r[2] = c->r[3] & 48u;
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[3] & 3u; if (_t) goto L_8003B5F4; }
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)2);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)208; if (_t) goto L_8003B5F4; }
    c->mem_w8((c->r[16] + (uint32_t)24), (uint8_t)c->r[2]); goto L_8003B68C;
  L_8003B5F4:;
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)13));
    c->r[2] = c->r[3] & 128u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_8003B62C; }
    c->r[4] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)583));
    c->r[4] = c->r[4] & 15u;
    c->r[31] = 0x8003B61Cu;
    c->r[4] = c->r[4] << 7; func_80083E80(c);
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 22);
    c->r[2] = c->r[2] + (uint32_t)48; goto L_8003B684;
  L_8003B62C:;
    c->r[2] = c->r[3] & 64u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)32; if (_t) goto L_8003B640; }
    c->mem_w8((c->r[16] + (uint32_t)24), (uint8_t)c->r[2]); goto L_8003B68C;
  L_8003B640:;
    c->r[2] = c->r[0] + (uint32_t)128;
    c->mem_w8((c->r[16] + (uint32_t)24), (uint8_t)c->r[2]); goto L_8003B68C;
  L_8003B64C:;
    c->r[2] = c->r[3] & 128u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_8003B67C; }
    c->r[4] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)583));
    c->r[4] = c->r[4] & 15u;
    c->r[31] = 0x8003B66Cu;
    c->r[4] = c->r[4] << 7; func_80083E80(c);
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 22);
    c->r[2] = c->r[2] + (uint32_t)16; goto L_8003B680;
  L_8003B67C:;
    c->r[2] = c->r[0] + c->r[0];
  L_8003B680:;
    c->r[2] = c->r[2] + (uint32_t)32;
  L_8003B684:;
    c->mem_w8((c->r[16] + (uint32_t)24), (uint8_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)32;
  L_8003B68C:;
    c->mem_w8((c->r[16] + (uint32_t)25), (uint8_t)c->r[2]);
    c->mem_w8((c->r[16] + (uint32_t)26), (uint8_t)c->r[2]); goto L_8003B69C;
  L_8003B698:;
    c->mem_w8((c->r[16] + (uint32_t)13), (uint8_t)c->r[0]);
  L_8003B69C:;
    c->r[31] = 0x8003B6A4u;
    c->r[4] = c->r[16] + c->r[0]; func_800597AC(c);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)1));
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_8003B6F0; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)382));
    c->r[17] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)8));
    c->r[2] = c->r[2] & 32u;
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_8003B6E4; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)377));
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_8003B6E4; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)9));
    c->mem_w8((c->r[16] + (uint32_t)8), (uint8_t)c->r[2]);
  L_8003B6E4:;
    c->r[31] = 0x8003B6ECu;
    c->r[4] = c->r[16] + c->r[0]; func_8003CCA4(c);
    c->mem_w8((c->r[16] + (uint32_t)8), (uint8_t)c->r[17]);
  L_8003B6F0:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

static void leaf_8003E030(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-72;
    c->r[2] = (uint32_t)32778u << 16;
    c->r[3] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)15116));
    c->r[2] = c->r[0] + (uint32_t)2;
    c->mem_w32((c->r[29] + (uint32_t)68), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)64), c->r[30]);
    c->mem_w32((c->r[29] + (uint32_t)60), c->r[23]);
    c->mem_w32((c->r[29] + (uint32_t)56), c->r[22]);
    c->mem_w32((c->r[29] + (uint32_t)52), c->r[21]);
    c->mem_w32((c->r[29] + (uint32_t)48), c->r[20]);
    c->mem_w32((c->r[29] + (uint32_t)44), c->r[19]);
    c->mem_w32((c->r[29] + (uint32_t)40), c->r[18]);
    c->mem_w32((c->r[29] + (uint32_t)36), c->r[17]);
    { int _t = (c->r[3] != c->r[2]); c->mem_w32((c->r[29] + (uint32_t)32), c->r[16]); if (_t) goto L_8003E234; }
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)148));
    c->r[17] = c->r[0] + (uint32_t)384;
    { int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[17]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[16] = (uint32_t)32778u << 16;
    c->r[3] = (uint32_t)32778u << 16;
    c->r[4] = c->mem_r32((c->r[16] + (uint32_t)15120));
    c->r[2] = c->mem_r32((c->r[3] + (uint32_t)15124));
    c->r[4] = c->r[4] + (uint32_t)136;
    c->mem_w32((c->r[16] + (uint32_t)15120), c->r[4]);
    c->r[2] = c->r[2] + (uint32_t)32;
    c->mem_w32((c->r[3] + (uint32_t)15124), c->r[2]);
    c->r[9] = c->lo;
    c->r[31] = 0x8003E0A8u;
    c->r[17] = (uint32_t)((int32_t)c->r[9] >> 8); func_80083F50(c);
    { int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[17]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[4] = c->mem_r32((c->r[16] + (uint32_t)15120));
    c->r[2] = c->lo;
    c->r[3] = c->r[2] << 1;
    c->r[3] = c->r[3] + c->r[2];
    c->r[3] = (uint32_t)((int32_t)c->r[3] >> 18);
    c->r[31] = 0x8003E0C8u;
    c->mem_w16((c->r[29] + (uint32_t)24), (uint16_t)c->r[3]); func_80083E80(c);
    { int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[17]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[17] = c->r[0] + c->r[0];
    c->r[2] = (uint32_t)32780u << 16;
    c->r[19] = c->r[2] + c->r[0];
    c->r[22] = c->r[0] + (uint32_t)128;
    c->r[20] = (uint32_t)255u << 16;
    c->r[20] = c->r[20] | 65535u;
    c->r[23] = (uint32_t)65280u << 16;
    c->r[16] = c->mem_r32((c->r[19] + (uint32_t)-2748));
    c->r[5] = c->r[0] + c->r[0];
    c->r[4] = c->r[16] + c->r[0];
    c->r[3] = c->lo;
    c->r[31] = 0x8003E100u;
    c->r[30] = (uint32_t)((int32_t)c->r[3] >> 18); func_80083DB0(c);
    c->r[6] = (uint32_t)255u << 16;
    c->r[6] = c->r[6] | 65535u;
    c->r[2] = (uint32_t)32783u << 16;
    c->r[21] = c->r[2] + c->r[0];
    c->r[7] = c->r[23] + c->r[0];
    c->r[2] = (uint32_t)8064u << 16;
    c->r[4] = c->mem_r32((c->r[16] + (uint32_t)0));
    c->r[3] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)309));
    c->r[5] = c->mem_r32((c->r[21] + (uint32_t)-10040));
    c->r[4] = c->r[4] & c->r[7];
    c->r[3] = (uint32_t)(c->r[3] < (uint32_t)1);
    c->r[2] = c->mem_r32((c->r[5] + (uint32_t)0));
    c->r[18] = c->r[3] << 8;
    c->r[2] = c->r[2] & c->r[6];
    c->r[4] = c->r[4] | c->r[2];
    c->r[6] = c->r[16] & c->r[6];
    c->mem_w32((c->r[16] + (uint32_t)0), c->r[4]);
    c->r[2] = c->mem_r32((c->r[5] + (uint32_t)0));
    c->r[16] = c->r[16] + (uint32_t)12;
    c->mem_w32((c->r[19] + (uint32_t)-2748), c->r[16]);
    c->r[2] = c->r[2] & c->r[7];
    c->r[2] = c->r[2] | c->r[6];
    c->mem_w32((c->r[5] + (uint32_t)0), c->r[2]);
  L_8003E15C:;
    c->r[16] = c->mem_r32((c->r[19] + (uint32_t)-2748));
    c->mem_w8((c->r[16] + (uint32_t)6), (uint8_t)c->r[22]);
    c->mem_w8((c->r[16] + (uint32_t)5), (uint8_t)c->r[22]);
    c->mem_w8((c->r[16] + (uint32_t)4), (uint8_t)c->r[22]);
    c->r[8] = (uint32_t)c->mem_r16((c->r[29] + (uint32_t)24));
    c->r[4] = c->r[16] + c->r[0];
    c->mem_w16((c->r[16] + (uint32_t)10), (uint16_t)c->r[30]);
    c->mem_w8((c->r[16] + (uint32_t)13), (uint8_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)12), (uint8_t)c->r[0]);
    c->mem_w16((c->r[16] + (uint32_t)14), (uint16_t)c->r[0]);
    c->r[2] = c->r[17] + c->r[8];
    c->mem_w16((c->r[16] + (uint32_t)8), (uint16_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)64;
    c->mem_w16((c->r[16] + (uint32_t)16), (uint16_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)256;
    c->r[31] = 0x8003E1A4u;
    c->mem_w16((c->r[16] + (uint32_t)18), (uint16_t)c->r[2]); func_80083CC0(c);
    c->r[4] = c->r[16] + c->r[0];
    c->r[31] = 0x8003E1B0u;
    c->r[5] = c->r[0] + (uint32_t)1; func_80083C70(c);
    c->r[4] = c->mem_r32((c->r[21] + (uint32_t)-10040));
    c->r[3] = c->mem_r32((c->r[16] + (uint32_t)0));
    c->r[2] = c->mem_r32((c->r[4] + (uint32_t)0));
    c->r[3] = c->r[3] & c->r[23];
    c->r[2] = c->r[2] & c->r[20];
    c->r[3] = c->r[3] | c->r[2];
    c->mem_w32((c->r[16] + (uint32_t)0), c->r[3]);
    c->r[3] = c->r[16] & c->r[20];
    c->r[2] = c->mem_r32((c->r[4] + (uint32_t)0));
    c->r[16] = c->r[16] + (uint32_t)20;
    c->mem_w32((c->r[19] + (uint32_t)-2748), c->r[16]);
    c->r[2] = c->r[2] & c->r[23];
    c->r[2] = c->r[2] | c->r[3];
    c->mem_w32((c->r[4] + (uint32_t)0), c->r[2]);
    c->r[2] = c->r[18] >> 4;
    c->r[4] = c->r[16] + c->r[0];
    c->r[5] = c->r[0] + (uint32_t)1;
    c->r[7] = c->r[17] & 1023u;
    c->r[7] = c->r[7] >> 6;
    c->r[7] = c->r[7] | 256u;
    c->r[6] = c->r[5] + c->r[0];
    c->r[7] = c->r[2] | c->r[7];
    c->r[31] = 0x8003E210u;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]); func_80083DE0(c);
    c->r[4] = c->mem_r32((c->r[21] + (uint32_t)-10040));
    c->r[31] = 0x8003E21Cu;
    c->r[5] = c->r[16] + c->r[0]; func_80083C30(c);
    c->r[16] = c->r[16] + (uint32_t)12;
    c->r[17] = c->r[17] + (uint32_t)64;
    c->r[2] = c->r[17] & 65535u;
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)320);
    { int _t = (c->r[2] != c->r[0]); c->mem_w32((c->r[19] + (uint32_t)-2748), c->r[16]); if (_t) goto L_8003E15C; }
  L_8003E234:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)68));
    c->r[30] = c->mem_r32((c->r[29] + (uint32_t)64));
    c->r[23] = c->mem_r32((c->r[29] + (uint32_t)60));
    c->r[22] = c->mem_r32((c->r[29] + (uint32_t)56));
    c->r[21] = c->mem_r32((c->r[29] + (uint32_t)52));
    c->r[20] = c->mem_r32((c->r[29] + (uint32_t)48));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)44));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)40));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)36));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[29] = c->r[29] + (uint32_t)72; return;
    return;
}

static void leaf_8003E264(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-72;
    c->mem_w32((c->r[29] + (uint32_t)36), c->r[17]);
    c->r[17] = c->r[0] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)48), c->r[20]);
    c->r[20] = (uint32_t)32780u << 16;
    c->mem_w32((c->r[29] + (uint32_t)52), c->r[21]);
    c->r[21] = c->r[0] + (uint32_t)128;
    c->mem_w32((c->r[29] + (uint32_t)56), c->r[22]);
    c->r[22] = (uint32_t)32783u << 16;
    c->mem_w32((c->r[29] + (uint32_t)44), c->r[19]);
    c->r[19] = (uint32_t)255u << 16;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)309));
    c->r[19] = c->r[19] | 65535u;
    c->mem_w32((c->r[29] + (uint32_t)60), c->r[23]);
    c->r[23] = (uint32_t)65280u << 16;
    c->mem_w32((c->r[29] + (uint32_t)64), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)40), c->r[18]);
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[16]);
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)1);
    c->r[18] = c->r[2] << 8;
  L_8003E2B8:;
    c->r[16] = c->mem_r32((c->r[20] + (uint32_t)-2748));
    c->r[2] = c->r[0] + (uint32_t)64;
    c->r[4] = c->r[16] + c->r[0];
    c->mem_w16((c->r[16] + (uint32_t)16), (uint16_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)256;
    c->mem_w8((c->r[16] + (uint32_t)6), (uint8_t)c->r[21]);
    c->mem_w8((c->r[16] + (uint32_t)5), (uint8_t)c->r[21]);
    c->mem_w8((c->r[16] + (uint32_t)4), (uint8_t)c->r[21]);
    c->mem_w16((c->r[16] + (uint32_t)8), (uint16_t)c->r[17]);
    c->mem_w16((c->r[16] + (uint32_t)10), (uint16_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)13), (uint8_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)12), (uint8_t)c->r[0]);
    c->mem_w16((c->r[16] + (uint32_t)14), (uint16_t)c->r[0]);
    c->r[31] = 0x8003E2F4u;
    c->mem_w16((c->r[16] + (uint32_t)18), (uint16_t)c->r[2]); func_80083CC0(c);
    c->r[4] = c->r[16] + c->r[0];
    c->r[31] = 0x8003E300u;
    c->r[5] = c->r[0] + c->r[0]; func_80083C70(c);
    c->r[4] = c->mem_r32((c->r[22] + (uint32_t)-10040));
    c->r[3] = c->mem_r32((c->r[16] + (uint32_t)0));
    c->r[2] = c->mem_r32((c->r[4] + (uint32_t)0));
    c->r[3] = c->r[3] & c->r[23];
    c->r[2] = c->r[2] & c->r[19];
    c->r[3] = c->r[3] | c->r[2];
    c->mem_w32((c->r[16] + (uint32_t)0), c->r[3]);
    c->r[3] = c->r[16] & c->r[19];
    c->r[2] = c->mem_r32((c->r[4] + (uint32_t)0));
    c->r[16] = c->r[16] + (uint32_t)20;
    c->mem_w32((c->r[20] + (uint32_t)-2748), c->r[16]);
    c->r[2] = c->r[2] & c->r[23];
    c->r[2] = c->r[2] | c->r[3];
    c->mem_w32((c->r[4] + (uint32_t)0), c->r[2]);
    c->r[2] = c->r[18] >> 4;
    c->r[4] = c->r[16] + c->r[0];
    c->r[5] = c->r[0] + (uint32_t)1;
    c->r[7] = c->r[17] & 1023u;
    c->r[7] = c->r[7] >> 6;
    c->r[7] = c->r[7] | 256u;
    c->r[6] = c->r[5] + c->r[0];
    c->r[7] = c->r[2] | c->r[7];
    c->r[31] = 0x8003E360u;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]); func_80083DE0(c);
    c->r[4] = c->mem_r32((c->r[22] + (uint32_t)-10040));
    c->r[31] = 0x8003E36Cu;
    c->r[5] = c->r[16] + c->r[0]; func_80083C30(c);
    c->r[16] = c->r[16] + (uint32_t)12;
    c->r[17] = c->r[17] + (uint32_t)64;
    c->r[2] = c->r[17] & 65535u;
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)320);
    { int _t = (c->r[2] != c->r[0]); c->mem_w32((c->r[20] + (uint32_t)-2748), c->r[16]); if (_t) goto L_8003E2B8; }
    c->r[17] = (uint32_t)32780u << 16;
    c->r[16] = c->mem_r32((c->r[17] + (uint32_t)-2748));
    c->r[5] = c->r[0] + (uint32_t)1;
    c->r[31] = 0x8003E398u;
    c->r[4] = c->r[16] + c->r[0]; func_80083DB0(c);
    c->r[5] = (uint32_t)255u << 16;
    c->r[5] = c->r[5] | 65535u;
    c->r[2] = (uint32_t)32783u << 16;
    c->r[7] = (uint32_t)65280u << 16;
    c->r[4] = c->r[0] + (uint32_t)6;
    c->r[6] = c->mem_r32((c->r[2] + (uint32_t)-10040));
    c->r[3] = c->mem_r32((c->r[16] + (uint32_t)0));
    c->r[2] = c->mem_r32((c->r[6] + (uint32_t)8188));
    c->r[3] = c->r[3] & c->r[7];
    c->r[2] = c->r[2] & c->r[5];
    c->r[3] = c->r[3] | c->r[2];
    c->mem_w32((c->r[16] + (uint32_t)0), c->r[3]);
    c->r[2] = c->mem_r32((c->r[6] + (uint32_t)8188));
    c->r[5] = c->r[16] & c->r[5];
    c->r[2] = c->r[2] & c->r[7];
    c->r[2] = c->r[2] | c->r[5];
    c->mem_w32((c->r[6] + (uint32_t)8188), c->r[2]);
    c->r[2] = (uint32_t)32782u << 16;
    c->r[2] = c->r[2] + (uint32_t)32384;
    c->r[3] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)46));
    c->r[5] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)50));
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)54));
    c->r[16] = c->r[16] + (uint32_t)12;
    c->mem_w32((c->r[17] + (uint32_t)-2748), c->r[16]);
    c->mem_w16((c->r[29] + (uint32_t)24), (uint16_t)c->r[3]);
    c->mem_w16((c->r[29] + (uint32_t)26), (uint16_t)c->r[5]);
    c->r[31] = 0x8003E408u;
    c->mem_w16((c->r[29] + (uint32_t)28), (uint16_t)c->r[2]); func_800329E0(c);
    c->r[2] = c->r[29] + (uint32_t)24;
    gte_write_data(0, c->mem_r32((c->r[2] + (uint32_t)0)));
    gte_write_data(1, c->mem_r32((c->r[2] + (uint32_t)4)));
    c->r[31] = 0x8003E41Cu;
    c->r[4] = c->r[0] + c->r[0]; func_800317CC(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)64));
    c->r[23] = c->mem_r32((c->r[29] + (uint32_t)60));
    c->r[22] = c->mem_r32((c->r[29] + (uint32_t)56));
    c->r[21] = c->mem_r32((c->r[29] + (uint32_t)52));
    c->r[20] = c->mem_r32((c->r[29] + (uint32_t)48));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)44));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)40));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)36));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[29] = c->r[29] + (uint32_t)72; return;
    return;
}

static void leaf_8003E448(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-88;
    c->r[11] = (uint32_t)32780u << 16;
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w32((c->r[29] + (uint32_t)48), c->r[16]);
    c->r[16] = c->mem_r32((c->r[11] + (uint32_t)-2748));
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)309));
    c->mem_w32((c->r[29] + (uint32_t)52), c->r[17]);
    c->r[17] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)56), c->r[18]);
    c->r[18] = c->r[5] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)84), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)80), c->r[30]);
    c->mem_w32((c->r[29] + (uint32_t)76), c->r[23]);
    c->mem_w32((c->r[29] + (uint32_t)72), c->r[22]);
    c->mem_w32((c->r[29] + (uint32_t)68), c->r[21]);
    c->mem_w32((c->r[29] + (uint32_t)64), c->r[20]);
    c->mem_w32((c->r[29] + (uint32_t)60), c->r[19]);
    c->r[4] = c->r[16] + c->r[0];
    c->r[3] = (uint32_t)(c->r[2] < (uint32_t)1);
    c->r[31] = 0x8003E49Cu;
    c->r[30] = c->r[3] << 8; func_80083CA0(c);
    c->r[4] = c->r[16] + c->r[0];
    c->r[31] = 0x8003E4A8u;
    c->r[5] = c->r[0] + (uint32_t)1; func_80083C70(c);
    c->r[20] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[17] + (uint32_t)0));
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[17] + (uint32_t)2));
    c->r[21] = (uint32_t)(int16_t)c->mem_r16((c->r[20] + (uint32_t)140));
    c->r[23] = c->r[20] + (uint32_t)140;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[2]);
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[3]);
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[17] + (uint32_t)0));
    c->r[5] = (uint32_t)(int16_t)c->mem_r16((c->r[17] + (uint32_t)4));
    c->r[7] = (uint32_t)(int16_t)c->mem_r16((c->r[17] + (uint32_t)2));
    c->r[6] = (uint32_t)(int16_t)c->mem_r16((c->r[17] + (uint32_t)6));
    c->r[8] = c->r[2] + c->r[0];
    c->r[3] = c->r[7] + c->r[0];
    c->r[2] = c->r[2] + c->r[5];
    c->r[3] = c->r[3] + c->r[6];
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[2]);
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[7]);
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[8]);
    c->mem_w32((c->r[29] + (uint32_t)36), c->r[3]);
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[17] + (uint32_t)0));
    c->r[5] = (uint32_t)(int16_t)c->mem_r16((c->r[17] + (uint32_t)4));
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[17] + (uint32_t)2));
    c->r[6] = (uint32_t)(int16_t)c->mem_r16((c->r[17] + (uint32_t)6));
    c->r[2] = c->r[2] + c->r[5];
    c->r[3] = c->r[3] + c->r[6];
    c->mem_w32((c->r[29] + (uint32_t)40), c->r[2]);
    c->mem_w32((c->r[29] + (uint32_t)44), c->r[3]);
    c->r[22] = (uint32_t)(int16_t)c->mem_r16((c->r[23] + (uint32_t)2));
    c->r[31] = 0x8003E520u;
    c->r[4] = c->r[18] + c->r[0]; func_80083F50(c);
    c->r[4] = c->r[18] + c->r[0];
    c->r[31] = 0x8003E52Cu;
    c->r[19] = c->r[2] + c->r[0]; func_80083E80(c);
    c->r[4] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[4] = c->r[4] - c->r[21];
    c->r[3] = (uint32_t)((int32_t)c->r[4] >> 7);
    c->r[4] = c->r[4] + c->r[3];
    { int64_t _p = (int64_t)(int32_t)c->r[4] * (int64_t)(int32_t)c->r[19]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[3] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[3] = c->r[3] - c->r[22];
    c->r[7] = c->lo;
    c->r[5] = (uint32_t)((int32_t)c->r[3] >> 7);
    c->r[3] = c->r[3] + c->r[5];
    { int64_t _p = (int64_t)(int32_t)c->r[3] * (int64_t)(int32_t)c->r[2]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[5] = c->lo;
    { int64_t _p = (int64_t)(int32_t)c->r[4] * (int64_t)(int32_t)c->r[2]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[6] = c->lo;
    { int64_t _p = (int64_t)(int32_t)c->r[3] * (int64_t)(int32_t)c->r[19]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[2] = c->r[7] + c->r[5];
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 12);
    c->r[2] = c->r[2] + c->r[21];
    c->r[21] = (uint32_t)(int16_t)c->mem_r16((c->r[20] + (uint32_t)140));
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[2]);
    c->r[3] = c->lo;
    c->r[3] = c->r[3] - c->r[6];
    c->r[3] = (uint32_t)((int32_t)c->r[3] >> 12);
    c->r[3] = c->r[3] + c->r[22];
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[3]);
    c->r[22] = (uint32_t)(int16_t)c->mem_r16((c->r[23] + (uint32_t)2));
    c->r[31] = 0x8003E5B4u;
    c->r[4] = c->r[18] + c->r[0]; func_80083F50(c);
    c->r[4] = c->r[18] + c->r[0];
    c->r[31] = 0x8003E5C0u;
    c->r[19] = c->r[2] + c->r[0]; func_80083E80(c);
    c->r[4] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[4] = c->r[4] - c->r[21];
    c->r[3] = (uint32_t)((int32_t)c->r[4] >> 7);
    c->r[4] = c->r[4] + c->r[3];
    { int64_t _p = (int64_t)(int32_t)c->r[4] * (int64_t)(int32_t)c->r[19]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[3] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[3] = c->r[3] - c->r[22];
    c->r[7] = c->lo;
    c->r[5] = (uint32_t)((int32_t)c->r[3] >> 7);
    c->r[3] = c->r[3] + c->r[5];
    { int64_t _p = (int64_t)(int32_t)c->r[3] * (int64_t)(int32_t)c->r[2]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[5] = c->lo;
    { int64_t _p = (int64_t)(int32_t)c->r[4] * (int64_t)(int32_t)c->r[2]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[6] = c->lo;
    { int64_t _p = (int64_t)(int32_t)c->r[3] * (int64_t)(int32_t)c->r[19]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[2] = c->r[7] + c->r[5];
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 12);
    c->r[2] = c->r[2] + c->r[21];
    c->r[21] = (uint32_t)(int16_t)c->mem_r16((c->r[20] + (uint32_t)140));
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[2]);
    c->r[3] = c->lo;
    c->r[3] = c->r[3] - c->r[6];
    c->r[3] = (uint32_t)((int32_t)c->r[3] >> 12);
    c->r[3] = c->r[3] + c->r[22];
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[3]);
    c->r[22] = (uint32_t)(int16_t)c->mem_r16((c->r[23] + (uint32_t)2));
    c->r[31] = 0x8003E648u;
    c->r[4] = c->r[18] + c->r[0]; func_80083F50(c);
    c->r[4] = c->r[18] + c->r[0];
    c->r[31] = 0x8003E654u;
    c->r[19] = c->r[2] + c->r[0]; func_80083E80(c);
    c->r[4] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[4] = c->r[4] - c->r[21];
    c->r[3] = (uint32_t)((int32_t)c->r[4] >> 7);
    c->r[4] = c->r[4] + c->r[3];
    { int64_t _p = (int64_t)(int32_t)c->r[4] * (int64_t)(int32_t)c->r[19]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[3] = c->mem_r32((c->r[29] + (uint32_t)36));
    c->r[3] = c->r[3] - c->r[22];
    c->r[7] = c->lo;
    c->r[5] = (uint32_t)((int32_t)c->r[3] >> 7);
    c->r[3] = c->r[3] + c->r[5];
    { int64_t _p = (int64_t)(int32_t)c->r[3] * (int64_t)(int32_t)c->r[2]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[5] = c->lo;
    { int64_t _p = (int64_t)(int32_t)c->r[4] * (int64_t)(int32_t)c->r[2]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[6] = c->lo;
    { int64_t _p = (int64_t)(int32_t)c->r[3] * (int64_t)(int32_t)c->r[19]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[2] = c->r[7] + c->r[5];
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 12);
    c->r[2] = c->r[2] + c->r[21];
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[2]);
    c->r[19] = (uint32_t)(int16_t)c->mem_r16((c->r[20] + (uint32_t)140));
    c->r[3] = c->lo;
    c->r[3] = c->r[3] - c->r[6];
    c->r[3] = (uint32_t)((int32_t)c->r[3] >> 12);
    c->r[3] = c->r[3] + c->r[22];
    c->mem_w32((c->r[29] + (uint32_t)36), c->r[3]);
    c->r[20] = (uint32_t)(int16_t)c->mem_r16((c->r[23] + (uint32_t)2));
    c->r[31] = 0x8003E6DCu;
    c->r[4] = c->r[18] + c->r[0]; func_80083F50(c);
    c->r[4] = c->r[18] + c->r[0];
    c->r[31] = 0x8003E6E8u;
    c->r[18] = c->r[2] + c->r[0]; func_80083E80(c);
    c->r[4] = c->mem_r32((c->r[29] + (uint32_t)40));
    c->r[4] = c->r[4] - c->r[19];
    c->r[3] = (uint32_t)((int32_t)c->r[4] >> 7);
    c->r[4] = c->r[4] + c->r[3];
    { int64_t _p = (int64_t)(int32_t)c->r[4] * (int64_t)(int32_t)c->r[18]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[3] = c->mem_r32((c->r[29] + (uint32_t)44));
    c->r[3] = c->r[3] - c->r[20];
    c->r[6] = c->lo;
    c->r[5] = (uint32_t)((int32_t)c->r[3] >> 7);
    c->r[3] = c->r[3] + c->r[5];
    { int64_t _p = (int64_t)(int32_t)c->r[3] * (int64_t)(int32_t)c->r[2]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[5] = c->lo;
    { int64_t _p = (int64_t)(int32_t)c->r[4] * (int64_t)(int32_t)c->r[2]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[4] = c->lo;
    { int64_t _p = (int64_t)(int32_t)c->r[3] * (int64_t)(int32_t)c->r[18]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[8] = (uint32_t)255u << 16;
    c->r[8] = c->r[8] | 65535u;
    c->r[7] = (uint32_t)c->mem_r16((c->r[29] + (uint32_t)36));
    c->r[2] = c->r[6] + c->r[5];
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 12);
    c->r[2] = c->r[2] + c->r[19];
    c->mem_w32((c->r[29] + (uint32_t)40), c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[29] + (uint32_t)16));
    c->r[5] = (uint32_t)c->mem_r16((c->r[29] + (uint32_t)28));
    c->r[6] = (uint32_t)c->mem_r16((c->r[29] + (uint32_t)32));
    c->r[9] = (uint32_t)c->mem_r16((c->r[29] + (uint32_t)40));
    c->r[3] = c->lo;
    c->r[3] = c->r[3] - c->r[4];
    c->r[3] = (uint32_t)((int32_t)c->r[3] >> 12);
    c->r[3] = c->r[3] + c->r[20];
    c->mem_w32((c->r[29] + (uint32_t)44), c->r[3]);
    c->r[3] = (uint32_t)c->mem_r16((c->r[29] + (uint32_t)20));
    c->r[4] = (uint32_t)c->mem_r16((c->r[29] + (uint32_t)24));
    c->r[10] = (uint32_t)c->mem_r16((c->r[29] + (uint32_t)44));
    c->r[11] = (uint32_t)32780u << 16;
    c->mem_w16((c->r[16] + (uint32_t)8), (uint16_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)63;
    c->mem_w8((c->r[16] + (uint32_t)36), (uint8_t)c->r[2]);
    c->mem_w8((c->r[16] + (uint32_t)20), (uint8_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)255;
    c->mem_w16((c->r[16] + (uint32_t)18), (uint16_t)c->r[5]);
    c->mem_w16((c->r[16] + (uint32_t)24), (uint16_t)c->r[6]);
    c->mem_w16((c->r[16] + (uint32_t)26), (uint16_t)c->r[7]);
    c->mem_w8((c->r[16] + (uint32_t)28), (uint8_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)12), (uint8_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)21), (uint8_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)13), (uint8_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)37), (uint8_t)c->r[2]);
    c->mem_w8((c->r[16] + (uint32_t)29), (uint8_t)c->r[2]);
    c->mem_w16((c->r[16] + (uint32_t)32), (uint16_t)c->r[9]);
    c->mem_w16((c->r[16] + (uint32_t)10), (uint16_t)c->r[3]);
    c->mem_w16((c->r[16] + (uint32_t)16), (uint16_t)c->r[4]);
    c->mem_w16((c->r[16] + (uint32_t)34), (uint16_t)c->r[10]);
    c->r[4] = (uint32_t)c->mem_r16((c->r[17] + (uint32_t)2));
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[17] + (uint32_t)2));
    c->r[2] = (uint32_t)c->mem_r16((c->r[17] + (uint32_t)0));
    c->r[5] = c->r[0] + (uint32_t)128;
    c->mem_w8((c->r[16] + (uint32_t)4), (uint8_t)c->r[5]);
    c->r[3] = c->r[30] + c->r[3];
    c->r[3] = c->r[3] & 256u;
    c->r[3] = (uint32_t)((int32_t)c->r[3] >> 4);
    c->r[2] = c->r[2] & 1023u;
    c->r[2] = c->r[2] >> 6;
    c->r[2] = c->r[2] | 256u;
    c->r[3] = c->r[3] | c->r[2];
    c->r[4] = c->r[4] + c->r[30];
    c->r[4] = c->r[4] & 512u;
    c->r[4] = c->r[4] << 2;
    c->r[3] = c->r[3] | c->r[4];
    c->r[2] = (uint32_t)32783u << 16;
    c->mem_w16((c->r[16] + (uint32_t)22), (uint16_t)c->r[3]);
    c->mem_w8((c->r[16] + (uint32_t)5), (uint8_t)c->r[5]);
    c->mem_w8((c->r[16] + (uint32_t)6), (uint8_t)c->r[5]);
    c->r[4] = c->mem_r32((c->r[2] + (uint32_t)-10040));
    c->r[3] = c->mem_r32((c->r[16] + (uint32_t)0));
    c->r[5] = (uint32_t)65280u << 16;
    c->mem_w16((c->r[16] + (uint32_t)14), (uint16_t)c->r[0]);
    c->r[2] = c->mem_r32((c->r[4] + (uint32_t)0));
    c->r[3] = c->r[3] & c->r[5];
    c->r[2] = c->r[2] & c->r[8];
    c->r[3] = c->r[3] | c->r[2];
    c->r[8] = c->r[16] & c->r[8];
    c->mem_w32((c->r[16] + (uint32_t)0), c->r[3]);
    c->r[2] = c->mem_r32((c->r[4] + (uint32_t)0));
    c->r[16] = c->r[16] + (uint32_t)40;
    c->mem_w32((c->r[11] + (uint32_t)-2748), c->r[16]);
    c->r[2] = c->r[2] & c->r[5];
    c->r[2] = c->r[2] | c->r[8];
    c->mem_w32((c->r[4] + (uint32_t)0), c->r[2]);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)84));
    c->r[30] = c->mem_r32((c->r[29] + (uint32_t)80));
    c->r[23] = c->mem_r32((c->r[29] + (uint32_t)76));
    c->r[22] = c->mem_r32((c->r[29] + (uint32_t)72));
    c->r[21] = c->mem_r32((c->r[29] + (uint32_t)68));
    c->r[20] = c->mem_r32((c->r[29] + (uint32_t)64));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)60));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)56));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)52));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)48));
    c->r[29] = c->r[29] + (uint32_t)88; return;
    return;
}

static void leaf_8003E894(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-72;
    c->r[2] = (uint32_t)32782u << 16;
    c->r[2] = c->r[2] + (uint32_t)32384;
    c->mem_w32((c->r[29] + (uint32_t)68), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)64), c->r[22]);
    c->mem_w32((c->r[29] + (uint32_t)60), c->r[21]);
    c->mem_w32((c->r[29] + (uint32_t)56), c->r[20]);
    c->mem_w32((c->r[29] + (uint32_t)52), c->r[19]);
    c->mem_w32((c->r[29] + (uint32_t)48), c->r[18]);
    c->mem_w32((c->r[29] + (uint32_t)44), c->r[17]);
    c->mem_w32((c->r[29] + (uint32_t)40), c->r[16]);
    c->r[3] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)46));
    c->r[5] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)50));
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)54));
    c->r[4] = c->r[0] + (uint32_t)6;
    c->mem_w16((c->r[29] + (uint32_t)24), (uint16_t)c->r[3]);
    c->mem_w16((c->r[29] + (uint32_t)26), (uint16_t)c->r[5]);
    c->r[31] = 0x8003E8E0u;
    c->mem_w16((c->r[29] + (uint32_t)28), (uint16_t)c->r[2]); func_800329E0(c);
    c->r[16] = (uint32_t)8064u << 16;
    c->r[18] = c->r[16] + (uint32_t)140;
    c->r[2] = c->r[29] + (uint32_t)24;
    gte_write_data(0, c->mem_r32((c->r[2] + (uint32_t)0)));
    gte_write_data(1, c->mem_r32((c->r[2] + (uint32_t)4)));
    c->r[31] = 0x8003E8FCu;
    c->r[4] = c->r[0] + c->r[0]; func_800317CC(c);
    c->r[20] = c->r[0] + c->r[0];
    c->r[22] = c->r[0] + (uint32_t)64;
    c->r[17] = (uint32_t)32778u << 16;
    c->r[4] = c->mem_r32((c->r[17] + (uint32_t)15128));
    c->r[31] = 0x8003E914u;
    c->r[21] = c->r[0] + (uint32_t)255; func_80083F50(c);
    c->r[2] = c->r[2] << 5;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 12);
    c->r[3] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)140));
    c->r[4] = c->mem_r32((c->r[17] + (uint32_t)15128));
    c->r[3] = c->r[3] + c->r[2];
    c->r[31] = 0x8003E930u;
    c->mem_w16((c->r[16] + (uint32_t)140), (uint16_t)c->r[3]); func_80083E80(c);
    c->r[19] = (uint32_t)32780u << 16;
    c->r[5] = c->r[20] + c->r[0];
    c->r[2] = c->r[2] << 5;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 12);
    c->r[16] = c->mem_r32((c->r[19] + (uint32_t)-2748));
    c->r[3] = (uint32_t)c->mem_r16((c->r[18] + (uint32_t)2));
    c->r[6] = c->mem_r32((c->r[17] + (uint32_t)15128));
    c->r[4] = c->r[16] + c->r[0];
    c->r[3] = c->r[3] + c->r[2];
    c->r[6] = c->r[6] + (uint32_t)585;
    c->mem_w16((c->r[18] + (uint32_t)2), (uint16_t)c->r[3]);
    c->r[31] = 0x8003E964u;
    c->mem_w32((c->r[17] + (uint32_t)15128), c->r[6]); func_80083DB0(c);
    c->r[4] = (uint32_t)255u << 16;
    c->r[4] = c->r[4] | 65535u;
    c->r[2] = (uint32_t)32783u << 16;
    c->r[6] = (uint32_t)65280u << 16;
    c->r[5] = c->mem_r32((c->r[2] + (uint32_t)-10040));
    c->r[3] = c->mem_r32((c->r[16] + (uint32_t)0));
    c->r[2] = c->mem_r32((c->r[5] + (uint32_t)0));
    c->r[3] = c->r[3] & c->r[6];
    c->r[2] = c->r[2] & c->r[4];
    c->r[3] = c->r[3] | c->r[2];
    c->r[4] = c->r[16] & c->r[4];
    c->mem_w32((c->r[16] + (uint32_t)0), c->r[3]);
    c->r[2] = c->mem_r32((c->r[5] + (uint32_t)0));
    c->r[16] = c->r[16] + (uint32_t)12;
    c->mem_w32((c->r[19] + (uint32_t)-2748), c->r[16]);
    c->r[2] = c->r[2] & c->r[6];
    c->r[2] = c->r[2] | c->r[4];
    c->mem_w32((c->r[5] + (uint32_t)0), c->r[2]);
    c->r[4] = c->r[29] + (uint32_t)32;
  L_8003E9B0:;
    c->r[5] = c->r[0] + (uint32_t)12;
    c->mem_w16((c->r[29] + (uint32_t)32), (uint16_t)c->r[20]);
    c->mem_w16((c->r[29] + (uint32_t)34), (uint16_t)c->r[0]);
    c->mem_w16((c->r[29] + (uint32_t)36), (uint16_t)c->r[22]);
    c->r[31] = 0x8003E9C8u;
    c->mem_w16((c->r[29] + (uint32_t)38), (uint16_t)c->r[21]); func_8003E448(c);
    c->r[20] = c->r[20] + (uint32_t)64;
    c->r[2] = (uint32_t)((int32_t)c->r[20] < 320);
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[29] + (uint32_t)32; if (_t) goto L_8003E9B0; }
    c->r[18] = (uint32_t)32780u << 16;
    c->r[5] = c->r[0] + c->r[0];
    c->r[6] = c->r[5] + c->r[0];
    c->r[16] = c->mem_r32((c->r[18] + (uint32_t)-2748));
    c->r[7] = c->r[5] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]);
    c->r[31] = 0x8003E9F8u;
    c->r[4] = c->r[16] + c->r[0]; func_80083DE0(c);
    c->r[5] = c->r[16] + c->r[0];
    c->r[17] = (uint32_t)32783u << 16;
    c->r[4] = c->mem_r32((c->r[17] + (uint32_t)-10040));
    c->r[31] = 0x8003EA0Cu;
    c->r[16] = c->r[16] + (uint32_t)12; func_80083C30(c);
    c->mem_w32((c->r[18] + (uint32_t)-2748), c->r[16]);
    c->r[4] = c->r[16] + c->r[0];
    c->r[31] = 0x8003EA1Cu;
    c->r[5] = c->r[0] + (uint32_t)1; func_80083DB0(c);
    c->r[5] = (uint32_t)255u << 16;
    c->r[5] = c->r[5] | 65535u;
    c->r[6] = (uint32_t)65280u << 16;
    c->r[4] = c->mem_r32((c->r[17] + (uint32_t)-10040));
    c->r[3] = c->mem_r32((c->r[16] + (uint32_t)0));
    c->r[2] = c->mem_r32((c->r[4] + (uint32_t)8188));
    c->r[3] = c->r[3] & c->r[6];
    c->r[2] = c->r[2] & c->r[5];
    c->r[3] = c->r[3] | c->r[2];
    c->mem_w32((c->r[16] + (uint32_t)0), c->r[3]);
    c->r[3] = c->r[16] + (uint32_t)12;
    c->r[2] = c->mem_r32((c->r[4] + (uint32_t)8188));
    c->r[16] = c->r[16] & c->r[5];
    c->mem_w32((c->r[18] + (uint32_t)-2748), c->r[3]);
    c->r[2] = c->r[2] & c->r[6];
    c->r[2] = c->r[2] | c->r[16];
    c->mem_w32((c->r[4] + (uint32_t)8188), c->r[2]);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)68));
    c->r[22] = c->mem_r32((c->r[29] + (uint32_t)64));
    c->r[21] = c->mem_r32((c->r[29] + (uint32_t)60));
    c->r[20] = c->mem_r32((c->r[29] + (uint32_t)56));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)52));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)48));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)44));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)40));
    c->r[29] = c->r[29] + (uint32_t)72; return;
    return;
}

static void leaf_8003EA88(Core* c) {
    c->r[2] = (uint32_t)32780u << 16;
    c->r[9] = c->mem_r32((c->r[2] + (uint32_t)-2748));
    c->r[8] = c->r[0] + c->r[0];
    c->r[11] = (uint32_t)65244u << 16;
    c->r[11] = c->r[11] | 47767u;
    c->r[10] = (uint32_t)255u << 16;
    c->r[10] = c->r[10] | 65535u;
    c->r[13] = (uint32_t)65280u << 16;
    c->r[6] = c->r[9] + (uint32_t)23;
  L_8003EAAC:;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)((int32_t)c->r[11] >> 16);
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)408));
    c->r[12] = (uint32_t)8064u << 16;
    c->r[2] = c->r[2] << 4;
    c->r[3] = c->r[3] + c->r[2];
    c->r[5] = c->r[3] & 1023u;
    c->r[2] = c->r[5] << 4;
    c->r[2] = c->r[2] - c->r[5];
    c->r[2] = c->r[2] << 6;
    c->r[5] = (uint32_t)((int32_t)c->r[2] >> 10);
    c->r[2] = c->mem_r32((c->r[12] + (uint32_t)140));
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 2);
    c->r[7] = c->r[2] + (uint32_t)64;
    c->r[3] = c->r[2] + (uint32_t)120;
    c->r[2] = (uint32_t)((int32_t)c->r[7] < 256);
    { int _t = (c->r[2] != c->r[0]); c->r[5] = c->r[5] + (uint32_t)-320; if (_t) goto L_8003EAFC; }
    c->r[7] = c->r[0] + (uint32_t)255;
  L_8003EAFC:;
    c->r[2] = (uint32_t)((int32_t)c->r[3] < 256);
    { int _t = (c->r[2] != c->r[0]); c->r[4] = (uint32_t)21845u << 16; if (_t) goto L_8003EB0C; }
    c->r[3] = c->r[0] + (uint32_t)255;
  L_8003EB0C:;
    c->r[4] = c->r[4] | 21845u;
    c->r[2] = c->r[3] << 8;
    c->r[2] = c->r[7] | c->r[2];
    c->r[3] = (uint32_t)255u << 16;
    c->r[2] = c->r[2] | c->r[3];
    c->mem_w32((c->r[6] + (uint32_t)-11), c->r[2]);
    c->r[2] = c->r[5] + (uint32_t)-320;
    c->mem_w16((c->r[6] + (uint32_t)-15), (uint16_t)c->r[2]);
    c->r[2] = c->r[5] + (uint32_t)320;
    c->mem_w16((c->r[6] + (uint32_t)1), (uint16_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)7;
    c->mem_w8((c->r[6] + (uint32_t)-20), (uint8_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)88;
    c->mem_w32((c->r[6] + (uint32_t)-19), c->r[10]);
    c->mem_w32((c->r[6] + (uint32_t)-3), c->r[10]);
    c->mem_w16((c->r[6] + (uint32_t)3), (uint16_t)c->r[8]);
    c->mem_w16((c->r[6] + (uint32_t)-5), (uint16_t)c->r[8]);
    c->mem_w16((c->r[6] + (uint32_t)-13), (uint16_t)c->r[8]);
    c->mem_w16((c->r[6] + (uint32_t)-7), (uint16_t)c->r[5]);
    c->mem_w8((c->r[6] + (uint32_t)-16), (uint8_t)c->r[2]);
    c->mem_w32((c->r[6] + (uint32_t)5), c->r[4]);
    c->mem_w8((c->r[6] + (uint32_t)0), (uint8_t)c->r[0]);
    c->r[6] = c->r[6] + (uint32_t)32;
    c->r[2] = (uint32_t)((int32_t)c->r[11] >> 2);
    c->r[11] = c->r[11] + c->r[2];
    c->r[8] = c->r[8] + (uint32_t)1;
    c->r[2] = (uint32_t)32783u << 16;
    c->r[4] = c->mem_r32((c->r[2] + (uint32_t)-10040));
    c->r[3] = c->mem_r32((c->r[9] + (uint32_t)0));
    c->r[2] = c->mem_r32((c->r[4] + (uint32_t)8188));
    c->r[3] = c->r[3] & c->r[13];
    c->r[2] = c->r[2] & c->r[10];
    c->r[3] = c->r[3] | c->r[2];
    c->mem_w32((c->r[9] + (uint32_t)0), c->r[3]);
    c->r[3] = c->r[9] & c->r[10];
    c->r[2] = c->mem_r32((c->r[4] + (uint32_t)8188));
    c->r[2] = c->r[2] & c->r[13];
    c->r[2] = c->r[2] | c->r[3];
    c->mem_w32((c->r[4] + (uint32_t)8188), c->r[2]);
    c->r[2] = (uint32_t)((int32_t)c->r[8] < 255);
    { int _t = (c->r[2] != c->r[0]); c->r[9] = c->r[9] + (uint32_t)32; if (_t) goto L_8003EAAC; }
    c->r[3] = c->mem_r32((c->r[12] + (uint32_t)140));
    c->r[2] = (uint32_t)32780u << 16;
    c->mem_w32((c->r[2] + (uint32_t)-2748), c->r[9]);
    c->r[2] = c->r[0] + (uint32_t)64;
    c->r[2] = c->r[2] - c->r[3];
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 5);
    c->r[3] = c->r[3] + c->r[2];
    c->r[3] = c->r[3] & 255u;
    c->mem_w32((c->r[12] + (uint32_t)140), c->r[3]); return;
    return;
}

static void leaf_8003EBE0(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-72;
    c->mem_w32((c->r[29] + (uint32_t)68), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)64), c->r[30]);
    c->mem_w32((c->r[29] + (uint32_t)60), c->r[23]);
    c->mem_w32((c->r[29] + (uint32_t)56), c->r[22]);
    c->mem_w32((c->r[29] + (uint32_t)52), c->r[21]);
    c->mem_w32((c->r[29] + (uint32_t)48), c->r[20]);
    c->mem_w32((c->r[29] + (uint32_t)44), c->r[19]);
    c->mem_w32((c->r[29] + (uint32_t)40), c->r[18]);
    c->mem_w32((c->r[29] + (uint32_t)36), c->r[17]);
    c->r[31] = 0x8003EC10u;
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[16]); func_8003E030(c);
    c->r[4] = (uint32_t)8064u << 16;
    c->r[2] = c->mem_r32((c->r[4] + (uint32_t)152));
    { int _t = (c->r[2] != c->r[0]); c->r[3] = (uint32_t)8064u << 16; if (_t) goto L_8003EC4C; }
    c->r[2] = c->mem_r32((c->r[3] + (uint32_t)148));
    c->r[2] = c->r[2] + (uint32_t)2;
    c->mem_w32((c->r[3] + (uint32_t)148), c->r[2]);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 256);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_8003EC64; }
    c->r[2] = c->r[0] + (uint32_t)1;
    c->mem_w32((c->r[4] + (uint32_t)152), c->r[2]); goto L_8003EC60;
  L_8003EC4C:;
    c->r[2] = c->mem_r32((c->r[3] + (uint32_t)148));
    { int _t = ((int32_t)c->r[2] <= 0); c->r[2] = c->r[2] + (uint32_t)-2; if (_t) goto L_8003EC60; }
    c->mem_w32((c->r[3] + (uint32_t)148), c->r[2]);
  L_8003EC60:;
    c->r[2] = (uint32_t)8064u << 16;
  L_8003EC64:;
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)144));
    c->r[4] = (uint32_t)8064u << 16;
    c->r[3] = c->r[2] + (uint32_t)-30;
    c->r[2] = (uint32_t)((int32_t)c->r[3] < 256);
    { int _t = (c->r[2] != c->r[0]); c->mem_w32((c->r[4] + (uint32_t)140), c->r[3]); if (_t) goto L_8003EC88; }
    c->r[2] = c->r[0] + (uint32_t)255;
    c->mem_w32((c->r[4] + (uint32_t)140), c->r[2]); goto L_8003EC94;
  L_8003EC88:;
    { int _t = ((int32_t)c->r[3] >= 0); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_8003EC98; }
    c->mem_w32((c->r[4] + (uint32_t)140), c->r[0]);
  L_8003EC94:;
    c->r[2] = (uint32_t)8064u << 16;
  L_8003EC98:;
    c->r[3] = c->mem_r32((c->r[2] + (uint32_t)144));
    c->r[4] = c->r[3] + c->r[0];
    c->r[3] = c->r[3] + (uint32_t)1;
    c->r[4] = (uint32_t)((int32_t)c->r[4] < 420);
    { int _t = (c->r[4] != c->r[0]); c->mem_w32((c->r[2] + (uint32_t)144), c->r[3]); if (_t) goto L_8003ECC8; }
    c->r[8] = c->r[0] + (uint32_t)1;
    c->r[2] = (uint32_t)32778u << 16;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[8]);
    c->mem_w8((c->r[2] + (uint32_t)15116), (uint8_t)c->r[0]); goto L_8003ECCC;
  L_8003ECC8:;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[0]);
  L_8003ECCC:;
    c->r[17] = c->r[0] + c->r[0];
    c->r[30] = (uint32_t)8064u << 16;
    c->r[21] = (uint32_t)32780u << 16;
    c->r[23] = (uint32_t)32783u << 16;
  L_8003ECDC:;
    { int _t = (c->r[17] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)256; if (_t) goto L_8003ECF0; }
    c->r[2] = c->mem_r32((c->r[30] + (uint32_t)140));
    c->r[4] = (uint32_t)((int32_t)c->r[2] >> 1); goto L_8003ED00;
  L_8003ECF0:;
    c->r[3] = c->mem_r32((c->r[30] + (uint32_t)140));
    c->r[2] = c->r[2] - c->r[3];
    c->r[4] = (uint32_t)((int32_t)c->r[2] >> 1);
  L_8003ED00:;
    c->r[2] = c->r[4] << 16;
    c->r[3] = c->r[4] << 8;
    c->r[2] = c->r[2] | c->r[3];
    c->r[22] = c->r[2] | c->r[4];
    c->r[20] = c->r[0] + c->r[0];
    c->r[19] = c->r[20] + c->r[0];
    c->r[18] = c->r[20] + c->r[0];
  L_8003ED1C:;
    c->r[2] = (uint32_t)256u << 16;
    c->r[16] = c->mem_r32((c->r[21] + (uint32_t)-2748));
    c->r[2] = c->r[2] | 128u;
    c->mem_w32((c->r[16] + (uint32_t)4), c->r[22]);
    c->mem_w32((c->r[16] + (uint32_t)16), c->r[2]);
    { int _t = (c->r[17] != c->r[0]); c->mem_w16((c->r[16] + (uint32_t)10), (uint16_t)c->r[0]); if (_t) goto L_8003ED68; }
    c->r[2] = (uint32_t)32744u << 16;
    { int _t = (c->r[20] != c->r[0]); c->mem_w32((c->r[16] + (uint32_t)12), c->r[2]); if (_t) goto L_8003ED5C; }
    c->r[2] = c->r[0] + (uint32_t)64;
    c->mem_w8((c->r[16] + (uint32_t)12), (uint8_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)64;
    c->mem_w16((c->r[16] + (uint32_t)16), (uint16_t)c->r[2]);
    c->mem_w16((c->r[16] + (uint32_t)8), (uint16_t)c->r[0]); goto L_8003ED74;
  L_8003ED5C:;
    c->r[2] = c->r[18] + (uint32_t)-64;
    c->mem_w16((c->r[16] + (uint32_t)8), (uint16_t)c->r[2]); goto L_8003ED74;
  L_8003ED68:;
    c->r[2] = (uint32_t)32680u << 16;
    c->mem_w32((c->r[16] + (uint32_t)12), c->r[2]);
    c->mem_w16((c->r[16] + (uint32_t)8), (uint16_t)c->r[18]);
  L_8003ED74:;
    c->r[31] = 0x8003ED7Cu;
    c->r[4] = c->r[16] + c->r[0]; func_80083CC0(c);
    { int _t = (c->r[17] != c->r[0]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_8003ED8C; }
    c->r[5] = c->r[0] + (uint32_t)1; goto L_8003ED90;
  L_8003ED8C:;
    c->r[5] = c->r[0] + c->r[0];
  L_8003ED90:;
    c->r[31] = 0x8003ED98u;
     func_80083C70(c);
    c->r[5] = c->r[16] + c->r[0];
    c->r[4] = c->mem_r32((c->r[23] + (uint32_t)-10040));
    c->r[31] = 0x8003EDA8u;
    c->r[16] = c->r[16] + (uint32_t)20; func_80083C30(c);
    { int _t = (c->r[17] != c->r[0]); c->mem_w32((c->r[21] + (uint32_t)-2748), c->r[16]); if (_t) goto L_8003EDB8; }
    c->r[7] = c->r[19] + (uint32_t)704; goto L_8003EDBC;
  L_8003EDB8:;
    c->r[7] = c->r[19] + (uint32_t)576;
  L_8003EDBC:;
    c->r[5] = c->r[0] + (uint32_t)1;
    c->r[7] = c->r[7] & 1023u;
    c->r[7] = (uint32_t)((int32_t)c->r[7] >> 6);
    c->r[6] = c->r[5] + c->r[0];
    c->r[16] = c->mem_r32((c->r[21] + (uint32_t)-2748));
    c->r[7] = c->r[7] | 176u;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]);
    c->r[31] = 0x8003EDE0u;
    c->r[4] = c->r[16] + c->r[0]; func_80083DE0(c);
    c->r[5] = c->r[16] + c->r[0];
    c->r[4] = c->mem_r32((c->r[23] + (uint32_t)-10040));
    c->r[31] = 0x8003EDF0u;
    c->r[16] = c->r[16] + (uint32_t)12; func_80083C30(c);
    c->mem_w32((c->r[21] + (uint32_t)-2748), c->r[16]);
    c->r[19] = c->r[19] + (uint32_t)64;
    c->r[20] = c->r[20] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[20] < 3);
    { int _t = (c->r[2] != c->r[0]); c->r[18] = c->r[18] + (uint32_t)128; if (_t) goto L_8003ED1C; }
    c->r[17] = c->r[17] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[17] < 2);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)32778u << 16; if (_t) goto L_8003ECDC; }
    c->r[3] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)15116));
    c->r[2] = (uint32_t)((int32_t)c->r[3] < 3);
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_8003EE8C; }
    { int _t = (c->r[3] == c->r[0]); c->r[17] = (uint32_t)32780u << 16; if (_t) goto L_8003EE8C; }
    c->r[16] = c->mem_r32((c->r[17] + (uint32_t)-2748));
    c->r[5] = c->r[0] + (uint32_t)1;
    c->r[31] = 0x8003EE44u;
    c->r[4] = c->r[16] + c->r[0]; func_80083DB0(c);
    c->r[4] = (uint32_t)255u << 16;
    c->r[4] = c->r[4] | 65535u;
    c->r[2] = (uint32_t)32783u << 16;
    c->r[6] = (uint32_t)65280u << 16;
    c->r[5] = c->mem_r32((c->r[2] + (uint32_t)-10040));
    c->r[3] = c->mem_r32((c->r[16] + (uint32_t)0));
    c->r[2] = c->mem_r32((c->r[5] + (uint32_t)8188));
    c->r[3] = c->r[3] & c->r[6];
    c->r[2] = c->r[2] & c->r[4];
    c->r[3] = c->r[3] | c->r[2];
    c->r[4] = c->r[16] & c->r[4];
    c->mem_w32((c->r[16] + (uint32_t)0), c->r[3]);
    c->r[2] = c->mem_r32((c->r[5] + (uint32_t)8188));
    c->r[16] = c->r[16] + (uint32_t)12;
    c->mem_w32((c->r[17] + (uint32_t)-2748), c->r[16]);
    c->r[2] = c->r[2] & c->r[6];
    c->r[2] = c->r[2] | c->r[4];
    c->mem_w32((c->r[5] + (uint32_t)8188), c->r[2]);
  L_8003EE8C:;
    c->r[2] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)68));
    c->r[30] = c->mem_r32((c->r[29] + (uint32_t)64));
    c->r[23] = c->mem_r32((c->r[29] + (uint32_t)60));
    c->r[22] = c->mem_r32((c->r[29] + (uint32_t)56));
    c->r[21] = c->mem_r32((c->r[29] + (uint32_t)52));
    c->r[20] = c->mem_r32((c->r[29] + (uint32_t)48));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)44));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)40));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)36));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[29] = c->r[29] + (uint32_t)72; return;
    return;
}

static void leaf_8003F024(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[17] = c->r[0] + c->r[0];
    c->r[2] = (uint32_t)32783u << 16;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[2] + (uint32_t)-15992;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[31]);
  L_8003F040:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)0));
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_8003F058; }
    c->r[31] = 0x8003F058u;
    c->r[4] = c->r[16] + c->r[0]; func_8003D23C(c);
  L_8003F058:;
    c->r[17] = c->r[17] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[17] < 40);
    { int _t = (c->r[2] != c->r[0]); c->r[16] = c->r[16] + (uint32_t)64; if (_t) goto L_8003F040; }
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

static void leaf_8003FA1C(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[31]);
    c->r[31] = 0x8003FA2Cu;
     func_8003B588(c);
    c->r[31] = 0x8003FA34u;
     func_8003C048(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_8003FA44(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[31]);
    c->r[31] = 0x8003FA54u;
     func_8004FD30(c);
    c->r[31] = 0x8003FA5Cu;
     func_80025D98(c);
    c->r[31] = 0x8003FA64u;
     func_8003BF00(c);
    c->r[31] = 0x8003FA6Cu;
     func_8003EEC0(c);
    c->r[31] = 0x8003FA74u;
     func_8003B588(c);
    c->r[31] = 0x8003FA7Cu;
     func_8003BB50(c);
    c->r[31] = 0x8003FA84u;
     func_8003BCF4(c);
    c->r[31] = 0x8003FA8Cu;
     func_8003C048(c);
    c->r[31] = 0x8003FA94u;
     func_8003F024(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[31]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)11));
    c->r[2] = c->r[3] & 64u;
    { int _t = (c->r[2] != c->r[0]); c->r[5] = c->r[0] + (uint32_t)80; if (_t) goto L_8003FAD0; }
    c->r[2] = c->r[3] & 128u;
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_8003FAD8; }
    c->r[5] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)128));
  L_8003FAD0:;
    c->r[31] = 0x8003FAD8u;
    c->r[6] = c->r[0] + c->r[0]; func_8002AE0C(c);
  L_8003FAD8:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
    c->r[18] = (uint32_t)32780u << 16;
    c->r[5] = c->r[5] << 16;
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[17] = c->mem_r32((c->r[18] + (uint32_t)-2748));
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16);
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[31]);
    c->r[31] = 0x8003FB18u;
    c->r[4] = c->r[17] + c->r[0]; func_80083DB0(c);
    c->r[4] = (uint32_t)255u << 16;
    c->r[4] = c->r[4] | 65535u;
    c->r[16] = c->r[16] << 16;
    c->r[2] = (uint32_t)32783u << 16;
    c->r[16] = (uint32_t)((int32_t)c->r[16] >> 14);
    c->r[5] = (uint32_t)65280u << 16;
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)-10040));
    c->r[3] = c->mem_r32((c->r[17] + (uint32_t)0));
    c->r[16] = c->r[16] + c->r[2];
    c->r[2] = c->mem_r32((c->r[16] + (uint32_t)0));
    c->r[3] = c->r[3] & c->r[5];
    c->r[2] = c->r[2] & c->r[4];
    c->r[3] = c->r[3] | c->r[2];
    c->r[4] = c->r[17] & c->r[4];
    c->mem_w32((c->r[17] + (uint32_t)0), c->r[3]);
    c->r[2] = c->mem_r32((c->r[16] + (uint32_t)0));
    c->r[17] = c->r[17] + (uint32_t)12;
    c->mem_w32((c->r[18] + (uint32_t)-2748), c->r[17]);
    c->r[2] = c->r[2] & c->r[5];
    c->r[2] = c->r[2] | c->r[4];
    c->mem_w32((c->r[16] + (uint32_t)0), c->r[2]);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

static void leaf_8003FB84(Core* c) {
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = c->r[0] + (uint32_t)255;
    c->mem_w32((c->r[3] + (uint32_t)140), c->r[2]); return;
    return;
}

static void leaf_8003FB94(Core* c) {
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w32((c->r[2] + (uint32_t)140), c->r[0]);
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w32((c->r[2] + (uint32_t)144), c->r[0]);
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w32((c->r[2] + (uint32_t)148), c->r[0]);
    c->r[2] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)32778u << 16;
    c->mem_w32((c->r[2] + (uint32_t)152), c->r[0]);
    c->r[2] = c->r[0] + (uint32_t)2;
    c->mem_w8((c->r[3] + (uint32_t)15116), (uint8_t)c->r[2]); return;
    return;
}

static void leaf_8003FBC4(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[31]);
    c->r[31] = 0x8003FBD8u;
    c->r[16] = c->r[4] + c->r[0]; func_8004766C(c);
    c->r[31] = 0x8003FBE0u;
    c->r[4] = c->r[16] + c->r[0]; func_80048750(c);
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)416));
    c->mem_w16((c->r[16] + (uint32_t)86), (uint16_t)c->r[2]);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_8003FC00(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[31]);
    c->r[31] = 0x8003FC14u;
    c->r[16] = c->r[4] + c->r[0]; func_8004766C(c);
    c->r[4] = c->r[16] + c->r[0];
  L_8003FC18:;
    c->r[5] = c->r[0] + c->r[0];
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)50));
    c->r[6] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)134));
    c->r[3] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)132));
    c->r[2] = c->r[2] + (uint32_t)10;
    c->r[6] = c->r[6] - c->r[3];
    c->r[6] = c->r[6] << 16;
    c->r[6] = (uint32_t)((int32_t)c->r[6] >> 16);
    c->r[31] = 0x8003FC40u;
    c->mem_w16((c->r[16] + (uint32_t)50), (uint16_t)c->r[2]); func_80049250(c);
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_8003FC18; }
    c->r[31] = 0x8003FC50u;
    c->r[4] = c->r[16] + c->r[0]; func_80048750(c);
    c->r[2] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)416));
    c->r[3] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)418));
    c->mem_w16((c->r[16] + (uint32_t)86), (uint16_t)c->r[2]);
    c->mem_w16((c->r[16] + (uint32_t)88), (uint16_t)c->r[3]);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_8003FC78(Core* c) {
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)42));
    c->mem_w16((c->r[4] + (uint32_t)88), (uint16_t)c->r[0]);
    c->r[2] = c->r[2] << 4;
    c->mem_w16((c->r[4] + (uint32_t)86), (uint16_t)c->r[2]); return;
    return;
}

static void leaf_8003FC8C(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[2] = (uint32_t)32780u << 16;
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[17] = c->r[2] + (uint32_t)-2040;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[31]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)14));
    c->r[2] = c->r[0] + (uint32_t)1;
    { int _t = (c->r[3] != c->r[2]); c->r[16] = c->r[4] + c->r[0]; if (_t) goto L_8003FCF8; }
    c->r[3] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)15));
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)106));
    { int _t = (c->r[3] != c->r[2]); c->r[2] = c->r[0] + c->r[0]; if (_t) goto L_8003FCFC; }
    c->r[31] = 0x8003FCD0u;
     func_8004766C(c);
    c->r[31] = 0x8003FCD8u;
    c->r[4] = c->r[16] + c->r[0]; func_80048750(c);
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)416));
    c->mem_w16((c->r[16] + (uint32_t)86), (uint16_t)c->r[2]);
    c->r[3] = (uint32_t)c->mem_r16((c->r[17] + (uint32_t)10));
    c->r[2] = c->r[0] + (uint32_t)1;
    c->mem_w16((c->r[16] + (uint32_t)50), (uint16_t)c->r[3]); goto L_8003FCFC;
  L_8003FCF8:;
    c->r[2] = c->r[0] + c->r[0];
  L_8003FCFC:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

static void leaf_8003FE00(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[31]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)3));
    c->r[2] = c->r[0] + (uint32_t)1;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = (uint32_t)((int32_t)c->r[3] < 2); if (_t) goto L_8003FE58; }
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)2; if (_t) goto L_8003FE38; }
    { int _t = (c->r[3] == c->r[0]); c->r[3] = c->r[0] + (uint32_t)1; if (_t) goto L_8003FE50; }
    c->r[2] = (uint32_t)32780u << 16; goto L_8003FE74;
  L_8003FE38:;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = c->r[0] + (uint32_t)3; if (_t) goto L_8003FE60; }
    { int _t = (c->r[3] == c->r[2]); c->r[3] = c->r[0] + (uint32_t)1; if (_t) goto L_8003FE68; }
    c->r[2] = (uint32_t)32780u << 16; goto L_8003FE74;
  L_8003FE50:;
    c->r[5] = c->r[0] + (uint32_t)29; goto L_8003FE6C;
  L_8003FE58:;
    c->r[5] = c->r[0] + (uint32_t)30; goto L_8003FE6C;
  L_8003FE60:;
    c->r[5] = c->r[0] + (uint32_t)31; goto L_8003FE6C;
  L_8003FE68:;
    c->r[5] = c->r[0] + (uint32_t)32;
  L_8003FE6C:;
    c->r[3] = c->r[0] + (uint32_t)1;
    c->r[2] = (uint32_t)32780u << 16;
  L_8003FE74:;
    c->r[2] = c->r[2] + (uint32_t)-1936;
    c->r[2] = c->r[5] + c->r[2];
    c->mem_w8((c->r[16] + (uint32_t)4), (uint8_t)c->r[3]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)580));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)2; if (_t) goto L_8003FEA8; }
    c->mem_w8((c->r[16] + (uint32_t)5), (uint8_t)c->r[3]);
    c->r[4] = c->r[0] + (uint32_t)43;
    c->r[31] = 0x8003FEA0u;
    c->r[5] = c->r[0] + (uint32_t)65; func_8004ED94(c);
     goto L_8003FEAC;
  L_8003FEA8:;
    c->mem_w8((c->r[16] + (uint32_t)5), (uint8_t)c->r[2]);
  L_8003FEAC:;
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)94));
    c->r[2] = c->r[0] + (uint32_t)2;
    { int _t = (c->r[3] != c->r[2]);  if (_t) goto L_8003FEC8; }
    c->r[2] = c->mem_r32((c->r[16] + (uint32_t)16));
    c->mem_w8((c->r[2] + (uint32_t)94), (uint8_t)c->r[0]);
  L_8003FEC8:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_8003FED8(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[31]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)6));
    { int _t = (c->r[3] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_8003FF08; }
    { int _t = (c->r[3] == c->r[2]); c->r[3] = c->r[0] + (uint32_t)-1; if (_t) goto L_8003FF38; }
     goto L_8003FFA0;
  L_8003FF08:;
    c->r[4] = c->r[0] + (uint32_t)25;
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x8003FF18u;
    c->r[6] = c->r[5] + c->r[0]; func_80074590(c);
    c->r[2] = c->r[0] + (uint32_t)1;
    c->mem_w8((c->r[16] + (uint32_t)0), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)6));
    c->r[3] = c->r[0] + (uint32_t)16;
    c->mem_w16((c->r[16] + (uint32_t)64), (uint16_t)c->r[3]);
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w8((c->r[16] + (uint32_t)6), (uint8_t)c->r[2]); goto L_8003FFA0;
  L_8003FF38:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)64));
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->mem_w16((c->r[16] + (uint32_t)64), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    { int _t = (c->r[2] != c->r[3]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_8003FF60; }
    c->mem_w8((c->r[16] + (uint32_t)5), (uint8_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)6), (uint8_t)c->r[0]);
  L_8003FF60:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)380));
    c->r[2] = c->r[2] & 1u;
    c->r[3] = c->r[2] << 1;
    c->r[3] = c->r[3] + c->r[2];
    c->r[2] = c->mem_r32((c->r[16] + (uint32_t)192));
    c->r[3] = c->r[3] << 1;
    c->r[31] = 0x8003FF84u;
    c->mem_w16((c->r[2] + (uint32_t)2), (uint16_t)c->r[3]); func_8009A450(c);
    c->r[2] = c->r[2] & 3u;
    c->r[2] = c->r[2] + (uint32_t)-2;
    c->r[3] = c->r[2] << 1;
    c->r[3] = c->r[3] + c->r[2];
    c->r[2] = c->mem_r32((c->r[16] + (uint32_t)192));
    c->r[3] = c->r[3] << 1;
    c->mem_w16((c->r[2] + (uint32_t)0), (uint16_t)c->r[3]);
  L_8003FFA0:;
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)94));
    c->r[2] = c->r[0] + (uint32_t)2;
    { int _t = (c->r[3] != c->r[2]);  if (_t) goto L_8003FFBC; }
    c->r[2] = c->mem_r32((c->r[16] + (uint32_t)16));
    c->mem_w8((c->r[2] + (uint32_t)94), (uint8_t)c->r[0]);
  L_8003FFBC:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_80041768(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[31]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)70));
    c->r[2] = c->r[5] & 255u;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_8004178C; }
    c->r[6] = c->r[6] << 16;
    c->r[31] = 0x8004178Cu;
    c->r[6] = (uint32_t)((int32_t)c->r[6] >> 16); func_80041718(c);
  L_8004178C:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_80044CD4(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-40;
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[17] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[19]);
    c->r[19] = c->r[5] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
    c->r[18] = c->r[6] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = (uint32_t)32799u << 16;
    c->r[16] = c->r[16] | 57456u;
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[31]);
    c->mem_w8((c->r[2] + (uint32_t)411), (uint8_t)c->r[0]);
    c->r[31] = 0x80044D10u;
    c->r[4] = c->r[0] + (uint32_t)2; func_80052010(c);
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)0));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + c->r[0]; if (_t) goto L_80044D70; }
    c->r[5] = c->r[18] + (uint32_t)3;
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w32((c->r[2] + (uint32_t)504), c->r[17]);
    c->r[2] = (uint32_t)32780u << 16;
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)-7936));
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = c->r[2] + c->r[19];
    { int _t = ((int32_t)c->r[5] >= 0); c->mem_w32((c->r[3] + (uint32_t)496), c->r[2]); if (_t) goto L_80044D48; }
    c->r[5] = c->r[18] + (uint32_t)6;
  L_80044D48:;
    c->r[4] = c->r[0] + (uint32_t)1;
    c->r[3] = (uint32_t)((int32_t)c->r[5] >> 2);
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w32((c->r[2] + (uint32_t)500), c->r[3]);
    c->r[2] = c->r[4] + c->r[0];
    c->r[5] = (uint32_t)32770u << 16;
    c->r[5] = c->r[5] + (uint32_t)-9416;
    c->r[31] = 0x80044D6Cu;
    c->mem_w8((c->r[16] + (uint32_t)108), (uint8_t)c->r[2]); func_80051F14(c);
    c->r[2] = c->r[0] + (uint32_t)1;
  L_80044D70:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)40; return;
    return;
}

static void leaf_80045CAC(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-88;
    c->mem_w32((c->r[29] + (uint32_t)68), c->r[21]);
    c->r[21] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)60), c->r[19]);
    c->r[19] = c->r[5] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)64), c->r[20]);
    c->r[20] = c->r[6] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)52), c->r[17]);
    c->r[17] = c->r[7] << 16;
    c->r[17] = (uint32_t)((int32_t)c->r[17] >> 16);
    c->r[4] = c->r[17] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)84), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)80), c->r[30]);
    c->mem_w32((c->r[29] + (uint32_t)76), c->r[23]);
    c->mem_w32((c->r[29] + (uint32_t)72), c->r[22]);
    c->mem_w32((c->r[29] + (uint32_t)56), c->r[18]);
    c->mem_w32((c->r[29] + (uint32_t)48), c->r[16]);
    c->r[31] = 0x80045CF8u;
    c->mem_w16((c->r[29] + (uint32_t)16), (uint16_t)c->r[7]); func_80083E80(c);
    c->r[2] = c->r[0] - c->r[2];
    c->r[16] = c->r[19] << 16;
    c->r[16] = (uint32_t)((int32_t)c->r[16] >> 16);
    { int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[16]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[8] = c->r[0] + (uint32_t)10;
    c->r[4] = c->r[17] + c->r[0];
    c->mem_w16((c->r[29] + (uint32_t)32), (uint16_t)c->r[0]);
    c->mem_w32((c->r[29] + (uint32_t)40), c->r[8]);
    c->r[8] = c->lo;
    c->r[31] = 0x80045D24u;
    c->r[18] = (uint32_t)((int32_t)c->r[8] >> 12); func_80083F50(c);
    { int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[16]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[23] = c->r[0] + c->r[0];
    c->r[30] = c->r[19] + c->r[0];
    c->r[19] = c->r[0] + c->r[0];
    c->r[16] = c->r[18] + c->r[0];
    c->r[18] = c->r[0] - c->r[18];
    c->mem_w16((c->r[29] + (uint32_t)24), (uint16_t)c->r[18]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[21] + (uint32_t)50));
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = c->r[2] + c->r[20];
    c->mem_w16((c->r[3] + (uint32_t)446), (uint16_t)c->r[2]);
    c->r[8] = c->lo;
    c->r[2] = (uint32_t)((int32_t)c->r[8] >> 12);
    c->r[20] = c->r[2] + c->r[0];
    c->r[18] = c->r[0] - c->r[2];
    c->r[4] = c->r[21] + c->r[0];
  L_80045D64:;
    c->r[8] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[21] + (uint32_t)46));
    c->r[3] = (uint32_t)c->mem_r16((c->r[21] + (uint32_t)54));
    c->r[2] = c->r[2] + c->r[20];
    c->r[3] = c->r[3] + c->r[16];
    c->mem_w16((c->r[8] + (uint32_t)444), (uint16_t)c->r[2]);
    c->r[8] = (uint32_t)8064u << 16;
    c->r[31] = 0x80045D88u;
    c->mem_w16((c->r[8] + (uint32_t)448), (uint16_t)c->r[3]); func_800498C8(c);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_80045FE8; }
    c->r[3] = (uint32_t)8064u << 16;
    c->r[5] = (uint32_t)8064u << 16;
    c->r[6] = (uint32_t)8064u << 16;
    c->r[7] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)440));
    c->r[4] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)436));
    c->r[2] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)438));
    c->r[5] = (uint32_t)c->mem_r16((c->r[6] + (uint32_t)434));
    c->r[4] = c->r[7] - c->r[4];
    c->r[31] = 0x80045DB8u;
    c->r[5] = c->r[2] - c->r[5]; func_80085690(c);
    c->r[3] = c->r[0] - c->r[2];
    c->r[2] = c->r[23] << 16;
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[22] << 16; if (_t) goto L_80045E30; }
    { int _t = (c->r[19] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)4; if (_t) goto L_80045E18; }
    c->r[8] = (uint32_t)c->mem_r16((c->r[29] + (uint32_t)16));
    c->r[2] = c->r[3] - c->r[8];
    c->r[2] = c->r[2] + (uint32_t)1024;
    c->r[3] = c->r[2] & 4095u;
    c->r[2] = c->r[30] << 16;
    { int _t = ((int32_t)c->r[2] >= 0); c->r[22] = c->r[3] + c->r[0]; if (_t) goto L_80045DFC; }
    c->r[30] = c->r[0] - c->r[30];
    c->r[2] = c->r[3] + (uint32_t)2048;
    c->r[22] = c->r[2] & 4095u;
  L_80045DFC:;
    c->r[2] = c->r[22] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 2048);
    { int _t = (c->r[2] != c->r[0]); c->r[22] = c->r[0] + (uint32_t)4; if (_t) goto L_80045E2C; }
    c->r[22] = c->r[0] + (uint32_t)8; goto L_80045E2C;
  L_80045E18:;
    c->r[3] = c->r[22] << 16;
    c->r[3] = (uint32_t)((int32_t)c->r[3] >> 16);
    { int _t = (c->r[3] != c->r[2]); c->r[22] = c->r[0] + (uint32_t)4; if (_t) goto L_80045E2C; }
    c->r[22] = c->r[0] + (uint32_t)8;
  L_80045E2C:;
    c->r[4] = c->r[22] << 16;
  L_80045E30:;
    c->r[4] = (uint32_t)((int32_t)c->r[4] >> 16);
    c->r[5] = c->r[0] + c->r[0];
    c->r[6] = c->r[23] << 16;
    c->r[31] = 0x80045E44u;
    c->r[6] = (uint32_t)((int32_t)c->r[6] >> 16); func_8004602C(c);
    c->r[3] = c->r[2] + c->r[0];
    c->r[2] = c->r[3] << 16;
    c->r[6] = (uint32_t)((int32_t)c->r[2] >> 16);
    { int _t = (c->r[6] == c->r[0]); c->r[23] = c->r[3] + c->r[0]; if (_t) goto L_80045FA8; }
    c->r[3] = c->r[3] & 32767u;
    c->r[4] = c->r[0] + (uint32_t)-64;
    c->r[8] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[8] + (uint32_t)444));
    c->r[8] = (uint32_t)8064u << 16;
    c->r[5] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[29] + (uint32_t)32), (uint16_t)c->r[3]);
    c->r[3] = (uint32_t)c->mem_r16((c->r[8] + (uint32_t)448));
    c->r[5] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)454));
    c->r[2] = c->r[2] & c->r[4];
    c->r[3] = c->r[3] & c->r[4];
    c->mem_w16((c->r[21] + (uint32_t)54), (uint16_t)c->r[3]);
    c->r[3] = (uint32_t)8064u << 16;
    c->r[4] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)450));
    c->r[3] = (uint32_t)c->mem_r16((c->r[21] + (uint32_t)54));
    c->mem_w16((c->r[21] + (uint32_t)46), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] + c->r[4];
    c->r[3] = c->r[3] + c->r[5];
    c->mem_w16((c->r[21] + (uint32_t)46), (uint16_t)c->r[2]);
    c->r[2] = c->r[6] & 32768u;
    { int _t = (c->r[2] == c->r[0]); c->mem_w16((c->r[21] + (uint32_t)54), (uint16_t)c->r[3]); if (_t) goto L_80045EC4; }
    c->r[8] = c->mem_r32((c->r[29] + (uint32_t)40));
    c->r[23] = c->r[0] + (uint32_t)1;
    c->r[8] = c->r[8] + (uint32_t)-1;
    c->mem_w32((c->r[29] + (uint32_t)40), c->r[8]); goto L_80045EC8;
  L_80045EC4:;
    c->r[23] = c->r[0] + c->r[0];
  L_80045EC8:;
    c->r[2] = c->r[23] << 16;
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[17] + c->r[0]; if (_t) goto L_80045F5C; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[21] + (uint32_t)46));
    c->r[3] = (uint32_t)c->mem_r16((c->r[21] + (uint32_t)54));
    c->r[2] = c->r[2] - c->r[20];
    c->r[3] = c->r[3] - c->r[16];
    c->mem_w16((c->r[21] + (uint32_t)46), (uint16_t)c->r[2]);
    c->r[31] = 0x80045EF0u;
    c->mem_w16((c->r[21] + (uint32_t)54), (uint16_t)c->r[3]); func_80083F50(c);
    c->r[16] = c->r[30] << 16;
    c->r[16] = (uint32_t)((int32_t)c->r[16] >> 16);
    { int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[16]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[4] = c->r[17] + c->r[0];
    c->r[8] = c->lo;
    c->r[31] = 0x80045F0Cu;
    c->r[20] = (uint32_t)((int32_t)c->r[8] >> 12); func_80083E80(c);
    c->r[2] = c->r[0] - c->r[2];
    { int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[16]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[2] = c->r[20] << 16;
    c->r[8] = c->lo;
    { int _t = ((int32_t)c->r[2] <= 0); c->r[16] = c->r[8] >> 12; if (_t) goto L_80045F2C; }
    c->r[20] = c->r[20] + (uint32_t)-1; goto L_80045F30;
  L_80045F2C:;
    c->r[20] = c->r[20] + (uint32_t)1;
  L_80045F30:;
    c->r[2] = c->r[16] << 16;
    { int _t = ((int32_t)c->r[2] <= 0);  if (_t) goto L_80045F44; }
    c->r[16] = c->r[16] + (uint32_t)-1; goto L_80045F48;
  L_80045F44:;
    c->r[16] = c->r[16] + (uint32_t)1;
  L_80045F48:;
    { int _t = (c->r[19] == c->r[0]);  if (_t) goto L_80045FB0; }
    c->r[20] = c->r[0] - c->r[20];
    c->r[16] = c->r[0] - c->r[16]; goto L_80045FB0;
  L_80045F5C:;
    c->r[2] = c->r[20] << 16;
    { int _t = ((int32_t)c->r[2] <= 0);  if (_t) goto L_80045F70; }
    c->r[20] = c->r[20] + (uint32_t)-1; goto L_80045F74;
  L_80045F70:;
    c->r[20] = c->r[20] + (uint32_t)1;
  L_80045F74:;
    c->r[2] = c->r[16] << 16;
    { int _t = ((int32_t)c->r[2] <= 0);  if (_t) goto L_80045F88; }
    c->r[16] = c->r[16] + (uint32_t)-1; goto L_80045F8C;
  L_80045F88:;
    c->r[16] = c->r[16] + (uint32_t)1;
  L_80045F8C:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[21] + (uint32_t)46));
    c->r[3] = (uint32_t)c->mem_r16((c->r[21] + (uint32_t)54));
    c->r[2] = c->r[2] - c->r[20];
    c->r[3] = c->r[3] - c->r[16];
    c->mem_w16((c->r[21] + (uint32_t)46), (uint16_t)c->r[2]);
    c->mem_w16((c->r[21] + (uint32_t)54), (uint16_t)c->r[3]); goto L_80045FB0;
  L_80045FA8:;
    c->r[20] = c->r[18] + c->r[0];
    c->r[16] = (uint32_t)c->mem_r16((c->r[29] + (uint32_t)24));
  L_80045FB0:;
    c->r[8] = c->mem_r32((c->r[29] + (uint32_t)40));
    { int _t = (c->r[8] == c->r[0]); c->r[2] = c->r[23] << 16; if (_t) goto L_80045FC8; }
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[21] + c->r[0]; if (_t) goto L_80045D64; }
  L_80045FC8:;
    c->r[8] = (uint32_t)c->mem_r16((c->r[29] + (uint32_t)32));
    { int _t = (c->r[8] != c->r[0]); c->r[3] = (uint32_t)8064u << 16; if (_t) goto L_80045FF0; }
    c->r[19] = c->r[19] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[19] < 2);
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[21] + c->r[0]; if (_t) goto L_80045D64; }
  L_80045FE8:;
    c->r[2] = c->r[0] + c->r[0]; goto L_80045FFC;
  L_80045FF0:;
    c->r[2] = c->r[0] + (uint32_t)1;
    c->mem_w8((c->r[3] + (uint32_t)600), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)(c->r[19] < (uint32_t)1);
  L_80045FFC:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)84));
    c->r[30] = c->mem_r32((c->r[29] + (uint32_t)80));
    c->r[23] = c->mem_r32((c->r[29] + (uint32_t)76));
    c->r[22] = c->mem_r32((c->r[29] + (uint32_t)72));
    c->r[21] = c->mem_r32((c->r[29] + (uint32_t)68));
    c->r[20] = c->mem_r32((c->r[29] + (uint32_t)64));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)60));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)56));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)52));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)48));
    c->r[29] = c->r[29] + (uint32_t)88; return;
    return;
}

static void leaf_800468AC(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-48;
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[20]);
    c->r[20] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[5] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
    c->r[18] = c->r[6] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[17] = c->r[7] << 16;
    c->r[17] = (uint32_t)((int32_t)c->r[17] >> 16);
    c->r[4] = c->r[17] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)44), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)40), c->r[22]);
    c->mem_w32((c->r[29] + (uint32_t)36), c->r[21]);
    c->r[31] = 0x800468ECu;
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[19]); func_80083E80(c);
    c->r[2] = c->r[0] - c->r[2];
    c->r[16] = c->r[16] << 16;
    c->r[16] = (uint32_t)((int32_t)c->r[16] >> 16);
    { int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[16]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[4] = c->r[17] + c->r[0];
    c->r[8] = c->lo;
    c->r[31] = 0x8004690Cu;
    c->r[19] = c->r[8] >> 12; func_80083F50(c);
    { int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[16]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[17] = c->r[0] + c->r[0];
    c->r[22] = (uint32_t)8064u << 16;
    c->r[21] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[20] + (uint32_t)50));
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = c->r[2] + c->r[18];
    c->mem_w16((c->r[3] + (uint32_t)446), (uint16_t)c->r[2]);
    c->r[8] = c->lo;
    c->r[18] = c->r[8] >> 12;
  L_80046934:;
    c->r[4] = c->r[20] + c->r[0];
    c->r[2] = (uint32_t)c->mem_r16((c->r[20] + (uint32_t)46));
    c->r[3] = (uint32_t)c->mem_r16((c->r[20] + (uint32_t)54));
    c->r[2] = c->r[2] + c->r[18];
    c->r[3] = c->r[3] + c->r[19];
    c->mem_w16((c->r[22] + (uint32_t)444), (uint16_t)c->r[2]);
    c->r[31] = 0x80046954u;
    c->mem_w16((c->r[21] + (uint32_t)448), (uint16_t)c->r[3]); func_800498C8(c);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_80046964; }
    c->r[2] = c->r[0] + c->r[0]; goto L_80046A1C;
  L_80046964:;
    c->r[3] = (uint32_t)8064u << 16;
    c->r[5] = (uint32_t)8064u << 16;
    c->r[6] = (uint32_t)8064u << 16;
    c->r[7] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)440));
    c->r[4] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)436));
    c->r[2] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)438));
    c->r[5] = (uint32_t)c->mem_r16((c->r[6] + (uint32_t)434));
    c->r[4] = c->r[7] - c->r[4];
    c->r[31] = 0x8004698Cu;
    c->r[5] = c->r[2] - c->r[5]; func_80085690(c);
    { int _t = (c->r[17] != c->r[0]); c->r[3] = c->r[0] - c->r[2]; if (_t) goto L_800469CC; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[20] + (uint32_t)86));
    c->r[2] = c->r[3] - c->r[2];
    c->r[2] = c->r[2] + (uint32_t)1024;
    c->r[2] = c->r[2] & 4095u;
    { int _t = ((int32_t)c->r[16] >= 0); c->r[4] = c->r[2] + c->r[0]; if (_t) goto L_800469B8; }
    c->r[2] = c->r[2] + (uint32_t)2048;
    c->r[4] = c->r[2] & 4095u;
  L_800469B8:;
    c->r[2] = (uint32_t)((int32_t)c->r[4] < 2048);
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[0] + (uint32_t)4; if (_t) goto L_800469E4; }
    c->r[4] = c->r[0] + (uint32_t)8; goto L_800469E4;
  L_800469CC:;
    c->r[2] = c->r[3] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[3] = c->r[0] + (uint32_t)4;
    { int _t = (c->r[2] != c->r[3]); c->r[4] = c->r[0] + (uint32_t)4; if (_t) goto L_800469E4; }
    c->r[4] = c->r[0] + (uint32_t)8;
  L_800469E4:;
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x800469F0u;
    c->r[6] = c->r[5] + c->r[0]; func_8004602C(c);
    c->r[2] = c->r[2] << 16;
    c->r[3] = (uint32_t)((int32_t)c->r[2] >> 16);
    { int _t = (c->r[3] != c->r[0]); c->r[18] = c->r[0] - c->r[18]; if (_t) goto L_80046A10; }
    c->r[17] = c->r[17] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[17] < 2);
    { int _t = (c->r[2] != c->r[0]); c->r[19] = c->r[0] - c->r[19]; if (_t) goto L_80046934; }
  L_80046A10:;
    { int _t = (c->r[3] == c->r[0]); c->r[2] = c->r[0] + c->r[0]; if (_t) goto L_80046A1C; }
    c->r[2] = (uint32_t)(c->r[17] < (uint32_t)1);
  L_80046A1C:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)44));
    c->r[22] = c->mem_r32((c->r[29] + (uint32_t)40));
    c->r[21] = c->mem_r32((c->r[29] + (uint32_t)36));
    c->r[20] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)48; return;
    return;
}

static void leaf_80047778(Core* c) {
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)456));
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[31]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)42));
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)0));
    { int _t = (c->r[3] == c->r[0]); c->r[17] = c->r[0] + (uint32_t)1; if (_t) goto L_800477B4; }
    c->r[2] = c->r[2] & 255u;
    c->r[2] = (uint32_t)(c->r[2] < c->r[3]);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_800477C4; }
  L_800477B4:;
    c->r[31] = 0x800477BCu;
    c->r[4] = c->r[16] + c->r[0]; func_800490E4(c);
    c->r[8] = (uint32_t)8064u << 16; goto L_80047858;
  L_800477C4:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)510));
    { int _t = (c->r[3] == c->r[2]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_800477E4; }
    c->r[4] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)42));
    c->r[31] = 0x800477E0u;
     func_80048ECC(c);
    c->r[2] = (uint32_t)8064u << 16;
  L_800477E4:;
    c->r[3] = (uint32_t)8064u << 16;
    c->r[5] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)430));
    c->r[4] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)432));
    c->r[2] = (uint32_t)(c->r[5] < c->r[4]);
    { int _t = (c->r[2] != c->r[0]); c->r[3] = (uint32_t)8064u << 16; if (_t) goto L_80047824; }
    c->r[2] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)8064u << 16;
    c->r[4] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)444));
    c->r[2] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)426));
    c->r[2] = c->r[4] - c->r[2];
    c->r[2] = c->r[2] & 65535u;
    c->r[2] = (uint32_t)(c->r[5] < c->r[2]); goto L_80047840;
  L_80047824:;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)448));
    c->r[3] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)428));
    c->r[2] = c->r[2] - c->r[3];
    c->r[2] = c->r[2] & 65535u;
    c->r[2] = (uint32_t)(c->r[4] < c->r[2]);
  L_80047840:;
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_80047854; }
    c->r[31] = 0x80047850u;
    c->r[5] = c->r[0] + c->r[0]; func_80048FC4(c);
    c->r[17] = c->r[2] + c->r[0];
  L_80047854:;
    c->r[8] = (uint32_t)8064u << 16;
  L_80047858:;
    c->r[4] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[8] + (uint32_t)430));
    c->r[3] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)432));
    c->r[2] = (uint32_t)(c->r[2] < c->r[3]);
    { int _t = (c->r[2] != c->r[0]); c->r[7] = (uint32_t)8064u << 16; if (_t) goto L_800478F8; }
    c->r[7] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)8064u << 16;
    c->r[6] = (uint32_t)(int16_t)c->mem_r16((c->r[7] + (uint32_t)444));
    c->r[5] = (uint32_t)(int16_t)c->mem_r16((c->r[3] + (uint32_t)426));
    c->r[3] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)426));
    c->r[2] = (uint32_t)((int32_t)c->r[6] < (int32_t)c->r[5]);
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_8004789C; }
    c->mem_w16((c->r[7] + (uint32_t)444), (uint16_t)c->r[3]); goto L_800478B8;
  L_8004789C:;
    c->r[4] = (uint32_t)c->mem_r16((c->r[8] + (uint32_t)430));
    c->r[2] = c->r[5] + c->r[4];
    c->r[2] = (uint32_t)((int32_t)c->r[2] < (int32_t)c->r[6]);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[3] + c->r[4]; if (_t) goto L_800478B8; }
    c->mem_w16((c->r[7] + (uint32_t)444), (uint16_t)c->r[2]);
  L_800478B8:;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)8064u << 16;
    c->r[4] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)444));
    c->r[3] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)434));
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)442));
    c->r[2] = c->r[2] - c->r[3];
    { int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[4]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[4] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)436));
    c->r[9] = c->lo;
    c->r[3] = (uint32_t)((int32_t)c->r[9] >> 14);
    c->r[2] = c->r[2] + c->r[3];
    c->mem_w16((c->r[4] + (uint32_t)448), (uint16_t)c->r[2]); goto L_80047974;
  L_800478F8:;
    c->r[3] = (uint32_t)8064u << 16;
    c->r[6] = (uint32_t)(int16_t)c->mem_r16((c->r[7] + (uint32_t)448));
    c->r[5] = (uint32_t)(int16_t)c->mem_r16((c->r[3] + (uint32_t)428));
    c->r[3] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)428));
    c->r[2] = (uint32_t)((int32_t)c->r[6] < (int32_t)c->r[5]);
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_8004791C; }
    c->mem_w16((c->r[7] + (uint32_t)448), (uint16_t)c->r[3]); goto L_80047938;
  L_8004791C:;
    c->r[4] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)432));
    c->r[2] = c->r[5] + c->r[4];
    c->r[2] = (uint32_t)((int32_t)c->r[2] < (int32_t)c->r[6]);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[3] + c->r[4]; if (_t) goto L_80047938; }
    c->mem_w16((c->r[7] + (uint32_t)448), (uint16_t)c->r[2]);
  L_80047938:;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)8064u << 16;
    c->r[4] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)448));
    c->r[3] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)436));
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)442));
    c->r[2] = c->r[2] - c->r[3];
    { int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[4]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[4] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)434));
    c->r[9] = c->lo;
    c->r[3] = (uint32_t)((int32_t)c->r[9] >> 14);
    c->r[2] = c->r[2] + c->r[3];
    c->mem_w16((c->r[4] + (uint32_t)444), (uint16_t)c->r[2]);
  L_80047974:;
    c->r[2] = c->r[17] + c->r[0];
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

static void leaf_80047B5C(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->r[8] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[5] + c->r[0];
    { int _t = (c->r[16] == c->r[0]); c->mem_w32((c->r[29] + (uint32_t)20), c->r[31]); if (_t) goto L_80047B90; }
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)484));
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)6));
    c->r[7] = c->r[0] + (uint32_t)-3;
    c->r[6] = c->r[2] >> 8; goto L_80047BA0;
  L_80047B90:;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)484));
    c->r[7] = c->r[0] + (uint32_t)-13;
    c->r[6] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)6));
  L_80047BA0:;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)8064u << 16;
    c->r[5] = c->mem_r32((c->r[2] + (uint32_t)484));
    c->r[4] = c->mem_r32((c->r[3] + (uint32_t)468));
    c->r[2] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)2));
    c->r[3] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)0));
    c->r[2] = c->r[2] << 3;
    c->r[4] = c->r[4] + c->r[2];
    c->r[2] = c->r[6] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 13);
    c->r[3] = c->r[3] & 1u;
    { int _t = (c->r[3] == c->r[0]); c->r[4] = c->r[4] + c->r[2]; if (_t) goto L_80047C10; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[8] + (uint32_t)50));
    c->r[3] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)4));
    c->r[2] = c->r[2] + (uint32_t)-128;
    c->r[6] = c->r[3] - c->r[2];
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)6));
    c->r[3] = c->r[6] & 65535u;
    c->r[2] = (uint32_t)(c->r[2] < c->r[3]);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + c->r[0]; if (_t) goto L_80047C10; }
    c->r[4] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)424));
    c->r[3] = c->r[3] & c->r[7];
    c->mem_w16((c->r[4] + (uint32_t)424), (uint16_t)c->r[3]); goto L_80047CAC;
  L_80047C10:;
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)0));
    c->r[3] = c->mem_r32((c->r[3] + (uint32_t)456));
    c->r[2] = c->r[2] << 1;
    c->r[2] = c->r[2] + c->r[3];
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)0));
    c->r[2] = c->r[2] << 1;
    c->r[3] = c->r[3] + c->r[2];
    c->r[6] = (uint32_t)(int16_t)c->mem_r16((c->r[3] + (uint32_t)6));
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[3] + (uint32_t)4));
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[3] + (uint32_t)2));
    c->r[5] = (uint32_t)(int16_t)c->mem_r16((c->r[3] + (uint32_t)0));
    c->r[4] = c->r[6] - c->r[4];
    c->r[31] = 0x80047C50u;
    c->r[5] = c->r[2] - c->r[5]; func_80085690(c);
    c->r[3] = (uint32_t)8064u << 16;
    c->r[3] = c->mem_r32((c->r[3] + (uint32_t)484));
    c->r[4] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)0));
    c->r[5] = c->r[0] - c->r[2];
    c->r[3] = c->r[3] & 16u;
    { int _t = (c->r[3] == c->r[0]); c->mem_w16((c->r[4] + (uint32_t)416), (uint16_t)c->r[5]); if (_t) goto L_80047C7C; }
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[2] + (uint32_t)418), (uint16_t)c->r[5]); goto L_80047C8C;
  L_80047C7C:;
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = c->r[5] + (uint32_t)2048;
    c->r[2] = c->r[2] & 4095u;
    c->mem_w16((c->r[3] + (uint32_t)418), (uint16_t)c->r[2]);
  L_80047C8C:;
    { int _t = (c->r[16] == c->r[0]); c->r[3] = (uint32_t)8064u << 16; if (_t) goto L_80047CA8; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)418));
    c->r[2] = c->r[2] + (uint32_t)2048;
    c->r[2] = c->r[2] & 4095u;
    c->mem_w16((c->r[3] + (uint32_t)418), (uint16_t)c->r[2]);
  L_80047CA8:;
    c->r[2] = c->r[0] + (uint32_t)1;
  L_80047CAC:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_80048654(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-40;
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[19]);
    c->r[19] = c->r[4] + c->r[0];
    c->r[2] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)8064u << 16;
    c->r[5] = (uint32_t)8064u << 16;
    c->r[6] = (uint32_t)8064u << 16;
    c->r[7] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)440));
    c->r[4] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)436));
    c->r[2] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)438));
    c->r[5] = (uint32_t)c->mem_r16((c->r[6] + (uint32_t)434));
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[4] = c->r[7] - c->r[4];
    c->r[31] = 0x8004869Cu;
    c->r[5] = c->r[2] - c->r[5]; func_80085690(c);
    c->r[4] = (uint32_t)8064u << 16;
    c->r[2] = c->r[0] - c->r[2];
    c->r[3] = (uint32_t)8064u << 16;
    c->r[3] = c->mem_r32((c->r[3] + (uint32_t)488));
    c->r[2] = c->r[2] & 4095u;
    c->mem_w16((c->r[4] + (uint32_t)416), (uint16_t)c->r[2]);
    c->r[17] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)0));
    c->r[17] = c->r[17] << 24;
    c->r[2] = (uint32_t)((int32_t)c->r[17] >> 24);
    { int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[2]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[18] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)2));
    c->r[4] = c->lo;
    c->r[18] = c->r[18] << 24;
    c->r[2] = (uint32_t)((int32_t)c->r[18] >> 24);
    { int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[2]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[16] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)1));
    c->r[16] = c->r[16] << 24;
    c->r[16] = (uint32_t)((int32_t)c->r[16] >> 24);
    c->r[17] = (uint32_t)((int32_t)c->r[17] >> 24);
    c->r[18] = c->r[2] + c->r[0];
    c->r[6] = c->lo;
    c->r[31] = 0x80048700u;
    c->r[4] = c->r[4] + c->r[6]; func_80084080(c);
    c->r[4] = c->r[16] << 16;
    c->r[4] = (uint32_t)((int32_t)c->r[4] >> 16);
    c->r[2] = c->r[2] << 16;
    c->r[31] = 0x80048714u;
    c->r[5] = (uint32_t)((int32_t)c->r[2] >> 16); func_80085690(c);
    c->r[17] = c->r[17] << 2;
    c->r[16] = c->r[16] << 2;
    c->r[18] = c->r[18] << 2;
    c->mem_w16((c->r[19] + (uint32_t)72), (uint16_t)c->r[17]);
    c->mem_w16((c->r[19] + (uint32_t)74), (uint16_t)c->r[16]);
    c->mem_w16((c->r[19] + (uint32_t)76), (uint16_t)c->r[18]);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[3] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[3] + (uint32_t)418), (uint16_t)c->r[2]);
    c->r[29] = c->r[29] + (uint32_t)40; return;
    return;
}

static void leaf_800489E4(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[17] = (uint32_t)8064u << 16;
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
    c->r[3] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)46));
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[2] + (uint32_t)446), (uint16_t)c->r[5]);
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[2] + (uint32_t)444), (uint16_t)c->r[3]);
    c->r[3] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)54));
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[2] + (uint32_t)448), (uint16_t)c->r[3]);
  L_80048A20:;
    c->r[31] = 0x80048A28u;
    c->r[4] = c->r[16] + c->r[0]; func_80047778(c);
    c->r[4] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)42));
    c->r[31] = 0x80048A34u;
     func_80049968(c);
    c->r[31] = 0x80048A3Cu;
     func_80047CBC(c);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + c->r[0]; if (_t) goto L_80048B18; }
    c->r[3] = c->mem_r32((c->r[17] + (uint32_t)480));
    c->r[2] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)0));
    c->r[2] = c->r[2] & 16384u;
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80048A88; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)0));
    c->mem_w8((c->r[16] + (uint32_t)42), (uint8_t)c->r[2]);
    c->r[2] = c->mem_r32((c->r[17] + (uint32_t)480));
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)0));
    c->r[2] = c->r[2] & 16384u;
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80048A20; }
  L_80048A88:;
    c->r[31] = 0x80048A90u;
     func_80048B30(c);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_80048B14; }
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)488));
    c->r[17] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)1));
    c->r[18] = (uint32_t)(int8_t)c->mem_r8((c->r[2] + (uint32_t)2));
    c->r[16] = (uint32_t)(int8_t)c->mem_r8((c->r[2] + (uint32_t)0));
    c->r[17] = c->r[17] << 24;
    c->r[17] = (uint32_t)((int32_t)c->r[17] >> 24);
    c->r[4] = c->r[18] + c->r[0];
    c->r[31] = 0x80048AC0u;
    c->r[5] = c->r[16] + c->r[0]; func_80085690(c);
    { int64_t _p = (int64_t)(int32_t)c->r[16] * (int64_t)(int32_t)c->r[16]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[4] = c->lo;
    { int64_t _p = (int64_t)(int32_t)c->r[18] * (int64_t)(int32_t)c->r[18]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = c->r[0] - c->r[2];
    c->r[2] = c->r[2] & 4095u;
    c->mem_w16((c->r[3] + (uint32_t)416), (uint16_t)c->r[2]);
    c->r[7] = c->lo;
    c->r[31] = 0x80048AF0u;
    c->r[4] = c->r[4] + c->r[7]; func_80084080(c);
    c->r[17] = c->r[17] << 16;
    c->r[4] = (uint32_t)((int32_t)c->r[17] >> 16);
    c->r[2] = c->r[2] << 16;
    c->r[31] = 0x80048B04u;
    c->r[5] = (uint32_t)((int32_t)c->r[2] >> 16); func_80085690(c);
    c->r[3] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[3] + (uint32_t)418), (uint16_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)1; goto L_80048B18;
  L_80048B14:;
    c->r[2] = c->r[0] + c->r[0];
  L_80048B18:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

static void leaf_80048B30(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[0] + (uint32_t)1;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[31]);
    c->r[31] = 0x80048B48u;
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]); func_80048360(c);
    c->r[6] = c->r[0] + c->r[0];
    c->r[7] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)8064u << 16;
    c->r[4] = (uint32_t)8064u << 16;
    c->r[5] = c->mem_r32((c->r[3] + (uint32_t)480));
    c->r[4] = c->mem_r32((c->r[4] + (uint32_t)472));
    c->r[3] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)2));
    c->r[10] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)4));
    c->r[3] = c->r[3] << 3;
    c->r[4] = c->r[4] + c->r[3];
    c->mem_w32((c->r[7] + (uint32_t)492), c->r[4]);
    { int _t = (c->r[10] == c->r[0]); c->r[4] = c->r[2] + c->r[0]; if (_t) goto L_80048CCC; }
    c->r[13] = c->r[5] + c->r[0];
    c->r[5] = (uint32_t)8064u << 16;
    c->r[2] = c->r[4] ^ 63u;
    c->r[2] = c->r[2] << 16;
    c->r[8] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[14] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)8064u << 16;
    c->r[12] = c->mem_r32((c->r[2] + (uint32_t)476));
    c->r[2] = c->r[4] << 16;
    c->r[9] = (uint32_t)(int16_t)c->mem_r16((c->r[3] + (uint32_t)450));
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[11] = c->r[9] - c->r[2];
  L_80048BB0:;
    c->r[4] = c->mem_r32((c->r[7] + (uint32_t)492));
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)0));
    c->r[2] = c->r[2] & 1u;
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80048CAC; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[13] + (uint32_t)0));
    c->r[2] = c->r[2] & 8u;
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80048BFC; }
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)4));
    { int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[9]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)2));
    c->r[15] = c->lo;
    c->r[3] = (uint32_t)((int32_t)c->r[15] >> 6); goto L_80048C44;
  L_80048BFC:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)4));
    { int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[11]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[3] = c->lo;
    cpu_div(c, c->r[3], c->r[8]);
    { int _t = (c->r[8] != c->r[0]);  if (_t) goto L_80048C24; }
    rec_break(c, 7168u);
  L_80048C24:;
    c->r[1] = c->r[0] + (uint32_t)-1;
    { int _t = (c->r[8] != c->r[1]); c->r[1] = (uint32_t)32768u << 16; if (_t) goto L_80048C3C; }
    { int _t = (c->r[3] != c->r[1]);  if (_t) goto L_80048C3C; }
    rec_break(c, 6144u);
  L_80048C3C:;
    c->r[3] = c->lo;
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)2));
  L_80048C44:;
    c->r[2] = c->r[2] + c->r[3];
    c->mem_w16((c->r[5] + (uint32_t)420), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[5] + (uint32_t)420));
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)446));
    c->r[3] = c->r[3] + (uint32_t)128;
    c->r[3] = (uint32_t)((int32_t)c->r[3] < (int32_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)420));
    { int _t = (c->r[3] == c->r[0]);  if (_t) goto L_80048C80; }
    { int _t = (c->r[16] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_80048CDC; }
    c->mem_w16((c->r[5] + (uint32_t)420), (uint16_t)c->r[17]); goto L_80048D28;
  L_80048C80:;
    c->r[17] = c->r[2] + c->r[0];
    c->r[2] = c->mem_r32((c->r[7] + (uint32_t)492));
    c->r[16] = c->r[0] + c->r[0];
    c->r[4] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)0));
    c->r[3] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)6));
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[2] + (uint32_t)422), (uint16_t)c->r[4]);
    c->r[2] = c->r[3] << 1;
    c->r[2] = c->r[2] + c->r[3];
    c->r[2] = c->r[12] + c->r[2];
    c->mem_w32((c->r[14] + (uint32_t)488), c->r[2]);
  L_80048CAC:;
    c->r[2] = c->mem_r32((c->r[7] + (uint32_t)492));
    c->r[6] = c->r[6] + (uint32_t)1;
    c->r[2] = c->r[2] + (uint32_t)8;
    c->mem_w32((c->r[7] + (uint32_t)492), c->r[2]);
    c->r[2] = c->r[10] & 65535u;
    c->r[2] = (uint32_t)((int32_t)c->r[6] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80048BB0; }
  L_80048CCC:;
    { int _t = (c->r[16] != c->r[0]); c->r[2] = c->r[0] + c->r[0]; if (_t) goto L_80048D18; }
    c->r[2] = c->r[0] + (uint32_t)1; goto L_80048D28;
  L_80048CDC:;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[5] = c->mem_r32((c->r[2] + (uint32_t)492));
    c->r[2] = c->r[0] + (uint32_t)1;
    c->r[3] = (uint32_t)8064u << 16;
    c->r[4] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)0));
    c->r[5] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)6));
    c->r[6] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[3] + (uint32_t)422), (uint16_t)c->r[4]);
    c->r[4] = (uint32_t)8064u << 16;
    c->r[3] = c->r[5] << (c->r[2] & 31);
    c->r[4] = c->mem_r32((c->r[4] + (uint32_t)476));
    c->r[3] = c->r[3] + c->r[5];
    c->r[4] = c->r[4] + c->r[3];
    c->mem_w32((c->r[6] + (uint32_t)488), c->r[4]); goto L_80048D28;
  L_80048D18:;
    c->r[3] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[3] + (uint32_t)422), (uint16_t)c->r[0]);
    c->r[3] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[3] + (uint32_t)420), (uint16_t)c->r[0]);
  L_80048D28:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

static void leaf_80048ECC(Core* c) {
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = c->r[4] & 255u;
    c->r[5] = c->mem_r32((c->r[3] + (uint32_t)456));
    c->r[2] = c->r[2] << 1;
    c->r[2] = c->r[2] + c->r[5];
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)0));
    c->r[3] = (uint32_t)8064u << 16;
    c->mem_w8((c->r[3] + (uint32_t)510), (uint8_t)c->r[4]);
    c->r[2] = c->r[2] << 1;
    c->r[5] = c->r[5] + c->r[2];
    c->r[6] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)0));
    c->r[4] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)2));
    c->r[3] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)4));
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[2] + (uint32_t)434), (uint16_t)c->r[6]);
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[2] + (uint32_t)438), (uint16_t)c->r[4]);
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[2] + (uint32_t)436), (uint16_t)c->r[3]);
    c->r[3] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)6));
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[2] + (uint32_t)440), (uint16_t)c->r[3]);
    c->r[3] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)8));
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[2] + (uint32_t)442), (uint16_t)c->r[3]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)10));
    c->r[3] = (uint32_t)8064u << 16;
    c->mem_w8((c->r[3] + (uint32_t)508), (uint8_t)c->r[2]);
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = c->r[2] >> 8;
    c->mem_w8((c->r[3] + (uint32_t)509), (uint8_t)c->r[2]);
    c->r[3] = c->r[6] & 65535u;
    c->r[2] = c->r[4] & 65535u;
    c->r[2] = (uint32_t)(c->r[2] < c->r[3]);
    { int _t = (c->r[2] != c->r[0]); c->r[3] = (uint32_t)8064u << 16; if (_t) goto L_80048F6C; }
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[2] + (uint32_t)426), (uint16_t)c->r[6]);
    c->r[2] = c->r[4] - c->r[6]; goto L_80048F78;
  L_80048F6C:;
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[2] + (uint32_t)426), (uint16_t)c->r[4]);
    c->r[2] = c->r[6] - c->r[4];
  L_80048F78:;
    c->mem_w16((c->r[3] + (uint32_t)430), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)8064u << 16;
    c->r[5] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)436));
    c->r[4] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)440));
    c->r[2] = (uint32_t)(c->r[4] < c->r[5]);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_80048FB0; }
    c->r[3] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[2] + (uint32_t)428), (uint16_t)c->r[5]);
    c->r[2] = c->r[4] - c->r[5];
    c->mem_w16((c->r[3] + (uint32_t)432), (uint16_t)c->r[2]); return;
  L_80048FB0:;
    c->r[3] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[2] + (uint32_t)428), (uint16_t)c->r[4]);
    c->r[2] = c->r[5] - c->r[4];
    c->mem_w16((c->r[3] + (uint32_t)432), (uint16_t)c->r[2]); return;
    return;
}

static void leaf_80048FC4(Core* c) {
    c->r[14] = c->r[4] + c->r[0];
    c->r[2] = (uint32_t)8064u << 16;
    c->r[12] = (uint32_t)8064u << 16;
    c->r[13] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)434));
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[12] + (uint32_t)444));
    c->r[2] = c->r[13] - c->r[4];
    { int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[2]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[9] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[9] + (uint32_t)448));
    c->r[2] = (uint32_t)8064u << 16;
    c->r[10] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)436));
    c->r[11] = c->lo;
    c->r[2] = c->r[10] - c->r[3];
    { int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[2]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[2] = (uint32_t)8064u << 16;
    c->r[8] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)438));
    c->r[7] = c->lo;
    c->r[4] = c->r[8] - c->r[4];
    { int64_t _p = (int64_t)(int32_t)c->r[4] * (int64_t)(int32_t)c->r[4]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[2] = (uint32_t)8064u << 16;
    c->r[6] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)440));
    c->r[4] = c->lo;
    c->r[3] = c->r[6] - c->r[3];
    { int64_t _p = (int64_t)(int32_t)c->r[3] * (int64_t)(int32_t)c->r[3]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->r[3] = c->r[11] + c->r[7];
    c->r[24] = c->lo;
    c->r[2] = c->r[4] + c->r[24];
    c->r[2] = (uint32_t)((int32_t)c->r[2] < (int32_t)c->r[3]);
    { int _t = (c->r[2] != c->r[0]); c->mem_w32((c->r[29] + (uint32_t)16), c->r[31]); if (_t) goto L_80049094; }
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)508));
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80049084; }
    { int _t = (c->r[5] != c->r[0]);  if (_t) goto L_80049070; }
    c->mem_w8((c->r[14] + (uint32_t)42), (uint8_t)c->r[2]);
  L_80049070:;
    c->r[4] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)508));
    c->r[31] = 0x8004907Cu;
     func_80048ECC(c);
    c->r[2] = c->r[0] + (uint32_t)1; goto L_800490D4;
  L_80049084:;
    c->r[2] = c->r[0] + (uint32_t)-1;
    c->mem_w16((c->r[12] + (uint32_t)444), (uint16_t)c->r[13]);
    c->mem_w16((c->r[9] + (uint32_t)448), (uint16_t)c->r[10]); goto L_800490D4;
  L_80049094:;
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)509));
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_800490C8; }
    { int _t = (c->r[5] != c->r[0]);  if (_t) goto L_800490B4; }
    c->mem_w8((c->r[14] + (uint32_t)42), (uint8_t)c->r[2]);
  L_800490B4:;
    c->r[4] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)509));
    c->r[31] = 0x800490C0u;
     func_80048ECC(c);
    c->r[2] = c->r[0] + (uint32_t)1; goto L_800490D4;
  L_800490C8:;
    c->r[2] = c->r[0] + (uint32_t)-1;
    c->mem_w16((c->r[12] + (uint32_t)444), (uint16_t)c->r[8]);
    c->mem_w16((c->r[9] + (uint32_t)448), (uint16_t)c->r[6]);
  L_800490D4:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_80049250(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->r[5] = c->r[5] << 16;
    c->r[6] = c->r[6] << 16;
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16);
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[31]);
    c->r[7] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)86));
    c->r[31] = 0x80049270u;
    c->r[6] = (uint32_t)((int32_t)c->r[6] >> 16); func_800455C0(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_80049280(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->r[5] = c->r[5] << 16;
    c->r[6] = c->r[6] << 16;
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16);
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[31]);
    c->r[7] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)86));
    c->r[31] = 0x800492A0u;
    c->r[6] = (uint32_t)((int32_t)c->r[6] >> 16); func_800492B0(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_8004954C(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->r[5] = c->r[5] << 16;
    c->r[6] = c->r[6] << 16;
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16);
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[31]);
    c->r[7] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)86));
    c->r[31] = 0x8004956Cu;
    c->r[6] = (uint32_t)((int32_t)c->r[6] >> 16); func_80045CAC(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->r[5] = c->r[5] << 16;
    c->r[6] = c->r[6] << 16;
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16);
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[31]);
    c->r[7] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)86));
    c->r[31] = 0x8004959Cu;
    c->r[6] = (uint32_t)((int32_t)c->r[6] >> 16); func_800468AC(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_80049674(Core* c) {
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)488));
    c->r[29] = c->r[29] + (uint32_t)-48;
    c->mem_w32((c->r[29] + (uint32_t)36), c->r[21]);
    c->r[21] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)40), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[20]);
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[19]);
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[18] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)1));
    c->r[17] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)2));
    c->r[16] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)0));
    c->r[18] = c->r[18] << 24;
    c->r[18] = (uint32_t)((int32_t)c->r[18] >> 24);
    c->r[17] = c->r[17] << 24;
    c->r[20] = (uint32_t)((int32_t)c->r[17] >> 24);
    c->r[16] = c->r[16] << 24;
    c->r[19] = (uint32_t)((int32_t)c->r[16] >> 24);
    c->r[17] = c->r[20] + c->r[0];
    c->r[4] = c->r[17] + c->r[0];
    c->r[16] = c->r[19] + c->r[0];
    c->r[31] = 0x800496D8u;
    c->r[5] = c->r[16] + c->r[0]; func_80085690(c);
    { int64_t _p = (int64_t)(int32_t)c->r[16] * (int64_t)(int32_t)c->r[16]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[4] = c->lo;
    { int64_t _p = (int64_t)(int32_t)c->r[17] * (int64_t)(int32_t)c->r[17]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = c->r[0] - c->r[2];
    c->r[2] = c->r[2] & 4095u;
    c->mem_w16((c->r[3] + (uint32_t)416), (uint16_t)c->r[2]);
    c->r[7] = c->lo;
    c->r[31] = 0x80049708u;
    c->r[4] = c->r[4] + c->r[7]; func_80084080(c);
    c->r[4] = c->r[18] << 16;
    c->r[4] = (uint32_t)((int32_t)c->r[4] >> 16);
    c->r[2] = c->r[2] << 16;
    c->r[31] = 0x8004971Cu;
    c->r[5] = (uint32_t)((int32_t)c->r[2] >> 16); func_80085690(c);
    c->r[19] = c->r[19] << 2;
    c->r[18] = c->r[18] << 2;
    c->r[20] = c->r[20] << 2;
    c->mem_w16((c->r[21] + (uint32_t)72), (uint16_t)c->r[19]);
    c->mem_w16((c->r[21] + (uint32_t)74), (uint16_t)c->r[18]);
    c->mem_w16((c->r[21] + (uint32_t)76), (uint16_t)c->r[20]);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)40));
    c->r[21] = c->mem_r32((c->r[29] + (uint32_t)36));
    c->r[20] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[3] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[3] + (uint32_t)418), (uint16_t)c->r[2]);
    c->r[29] = c->r[29] + (uint32_t)48; return;
    return;
}

static void leaf_80049760(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
    c->r[18] = c->r[4] + c->r[0];
    c->r[2] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)8064u << 16;
    c->r[5] = (uint32_t)8064u << 16;
    c->r[6] = (uint32_t)8064u << 16;
    c->r[7] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)440));
    c->r[4] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)436));
    c->r[2] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)438));
    c->r[5] = (uint32_t)c->mem_r16((c->r[6] + (uint32_t)434));
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[4] = c->r[7] - c->r[4];
    c->r[31] = 0x800497A4u;
    c->r[5] = c->r[2] - c->r[5]; func_80085690(c);
    c->r[4] = c->r[2] & 4095u;
    c->r[17] = (uint32_t)8064u << 16;
    c->r[31] = 0x800497B4u;
    c->mem_w16((c->r[17] + (uint32_t)416), (uint16_t)c->r[4]); func_80083F50(c);
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[17] + (uint32_t)416));
    c->r[31] = 0x800497C0u;
    c->r[16] = c->r[2] + c->r[0]; func_80083E80(c);
    c->r[3] = (uint32_t)8064u << 16;
    c->r[16] = (uint32_t)((int32_t)c->r[16] >> 4);
    c->mem_w16((c->r[3] + (uint32_t)418), (uint16_t)c->r[0]);
    c->r[3] = (uint32_t)c->mem_r16((c->r[17] + (uint32_t)416));
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 4);
    c->mem_w16((c->r[18] + (uint32_t)72), (uint16_t)c->r[16]);
    c->mem_w16((c->r[18] + (uint32_t)76), (uint16_t)c->r[2]);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[3] = c->r[0] - c->r[3];
    c->r[3] = c->r[3] & 4095u;
    c->mem_w16((c->r[17] + (uint32_t)416), (uint16_t)c->r[3]);
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

static void leaf_80049F80(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->r[2] = (uint32_t)32782u << 16;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[31]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)7));
    { int _t = (c->r[3] == c->r[0]); c->r[7] = c->r[2] + (uint32_t)32384; if (_t) goto L_80049FB0; }
    c->r[2] = c->r[0] + (uint32_t)1;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = c->r[0] + c->r[0]; if (_t) goto L_8004A040; }
     goto L_8004A108;
  L_80049FB0:;
    c->r[2] = (uint32_t)32780u << 16;
    c->r[5] = c->r[2] + (uint32_t)-1936;
    c->r[6] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)12));
    c->r[2] = c->r[0] + (uint32_t)16;
    { int _t = (c->r[6] != c->r[2]);  if (_t) goto L_80049FDC; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)13));
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w8((c->r[5] + (uint32_t)13), (uint8_t)c->r[2]); goto L_8004A000;
  L_80049FDC:;
    c->r[3] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)13));
    c->r[2] = (uint32_t)(c->r[3] < (uint32_t)8);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[3] + (uint32_t)1; if (_t) goto L_80049FF8; }
    c->mem_w8((c->r[5] + (uint32_t)13), (uint8_t)c->r[2]); goto L_8004A000;
  L_80049FF8:;
    c->r[2] = c->r[6] + (uint32_t)1;
    c->mem_w8((c->r[5] + (uint32_t)12), (uint8_t)c->r[2]);
  L_8004A000:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)7));
    c->mem_w16((c->r[4] + (uint32_t)64), (uint16_t)c->r[0]);
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w8((c->r[4] + (uint32_t)7), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)32780u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-1923));
    c->r[3] = (uint32_t)c->mem_r16((c->r[7] + (uint32_t)366));
    c->r[2] = c->r[2] - c->r[3];
    c->r[3] = (uint32_t)32783u << 16;
    c->r[3] = c->r[3] + (uint32_t)-12200;
    c->mem_w16((c->r[4] + (uint32_t)66), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)9));
    c->r[2] = c->r[2] | 2u;
    c->mem_w8((c->r[3] + (uint32_t)9), (uint8_t)c->r[2]);
  L_8004A040:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)66));
    c->r[5] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)66));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)32780u << 16; if (_t) goto L_8004A074; }
    c->r[3] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-1923));
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[7] + (uint32_t)366));
    { int _t = (c->r[3] != c->r[2]);  if (_t) goto L_8004A094; }
    c->r[31] = 0x8004A06Cu;
    c->r[5] = c->r[0] + c->r[0]; func_80072114(c);
    c->r[4] = (uint32_t)32783u << 16; goto L_8004A098;
  L_8004A074:;
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[7] + (uint32_t)366));
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-1923));
    c->r[2] = (uint32_t)((int32_t)c->r[3] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_8004A0B0; }
    c->mem_w16((c->r[4] + (uint32_t)66), (uint16_t)c->r[0]);
    c->mem_w16((c->r[4] + (uint32_t)64), (uint16_t)c->r[0]);
  L_8004A094:;
    c->r[4] = (uint32_t)32783u << 16;
  L_8004A098:;
    c->r[4] = c->r[4] + (uint32_t)-12200;
    c->r[3] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)9));
    c->r[2] = c->r[0] + (uint32_t)1;
    c->r[3] = c->r[3] & 253u;
    c->mem_w8((c->r[4] + (uint32_t)9), (uint8_t)c->r[3]); goto L_8004A108;
  L_8004A0B0:;
    { int _t = ((int32_t)c->r[3] > 0); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_8004A0C4; }
    c->mem_w16((c->r[4] + (uint32_t)66), (uint16_t)c->r[0]);
    c->mem_w16((c->r[4] + (uint32_t)64), (uint16_t)c->r[0]); goto L_8004A108;
  L_8004A0C4:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)64));
    c->r[3] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)64));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[3] + (uint32_t)-1; if (_t) goto L_8004A100; }
    c->r[2] = c->r[5] + (uint32_t)-1;
    c->mem_w16((c->r[4] + (uint32_t)66), (uint16_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)4;
    c->mem_w16((c->r[4] + (uint32_t)64), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[7] + (uint32_t)368));
    c->r[3] = (uint32_t)c->mem_r16((c->r[7] + (uint32_t)366));
    c->r[2] = c->r[2] + (uint32_t)1;
    c->r[3] = c->r[3] + (uint32_t)1;
    c->mem_w16((c->r[7] + (uint32_t)368), (uint16_t)c->r[2]);
    c->mem_w16((c->r[7] + (uint32_t)366), (uint16_t)c->r[3]); goto L_8004A104;
  L_8004A100:;
    c->mem_w16((c->r[4] + (uint32_t)64), (uint16_t)c->r[2]);
  L_8004A104:;
    c->r[2] = c->r[0] + c->r[0];
  L_8004A108:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_8004A118(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[31]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)6));
    { int _t = (c->r[3] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_8004A148; }
    { int _t = (c->r[3] == c->r[2]); c->r[2] = c->r[0] + c->r[0]; if (_t) goto L_8004A264; }
     goto L_8004A290;
  L_8004A148:;
    c->r[2] = (uint32_t)32782u << 16;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)32750));
    { int _t = ((int32_t)c->r[2] <= 0); c->r[4] = c->r[0] + (uint32_t)31; if (_t) goto L_8004A28C; }
    c->r[31] = 0x8004A164u;
    c->r[5] = c->r[0] + (uint32_t)-140; func_800310F4(c);
    c->r[3] = c->r[2] + c->r[0];
    { int _t = (c->r[3] == c->r[0]); c->r[4] = c->r[0] + (uint32_t)40; if (_t) goto L_8004A1A4; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)40));
    c->r[2] = c->r[2] | 128u;
    c->mem_w8((c->r[3] + (uint32_t)40), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)46));
    c->mem_w16((c->r[3] + (uint32_t)44), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)50));
    c->mem_w16((c->r[3] + (uint32_t)46), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)54));
    c->mem_w16((c->r[3] + (uint32_t)48), (uint16_t)c->r[2]);
  L_8004A1A4:;
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x8004A1B0u;
    c->r[6] = c->r[5] + c->r[0]; func_80074590(c);
    c->r[2] = (uint32_t)32780u << 16;
    c->r[4] = c->r[2] + (uint32_t)-1936;
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)18));
    c->r[2] = c->r[2] + (uint32_t)1;
    c->r[3] = c->r[2] & 255u;
    c->mem_w8((c->r[4] + (uint32_t)18), (uint8_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)1;
    { int _t = (c->r[3] != c->r[2]); c->r[2] = c->r[0] + (uint32_t)2; if (_t) goto L_8004A1EC; }
    c->r[4] = c->r[0] + (uint32_t)23;
    c->r[31] = 0x8004A1E4u;
    c->r[5] = c->r[0] + (uint32_t)65; func_8004ED94(c);
    c->r[2] = c->r[0] + (uint32_t)1; goto L_8004A290;
  L_8004A1EC:;
    { int _t = (c->r[3] != c->r[2]); c->r[2] = c->r[0] + (uint32_t)255; if (_t) goto L_8004A264; }
    c->r[3] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)174));
    { int _t = (c->r[3] == c->r[2]);  if (_t) goto L_8004A218; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)13));
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)8);
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[0] + (uint32_t)23; if (_t) goto L_8004A21C; }
  L_8004A218:;
    c->r[4] = c->r[0] + (uint32_t)14;
  L_8004A21C:;
    c->r[31] = 0x8004A224u;
    c->r[5] = c->r[0] + (uint32_t)65; func_8004ED94(c);
    c->r[3] = (uint32_t)32780u << 16;
    c->r[3] = c->r[3] + (uint32_t)-1936;
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)18));
    c->r[2] = c->r[2] + (uint32_t)-2;
    c->mem_w8((c->r[3] + (uint32_t)18), (uint8_t)c->r[2]);
    c->r[3] = (uint32_t)32783u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)6));
    c->r[3] = c->r[3] + (uint32_t)-12200;
    c->mem_w8((c->r[16] + (uint32_t)7), (uint8_t)c->r[0]);
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w8((c->r[16] + (uint32_t)6), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)9));
    c->r[2] = c->r[2] | 2u;
    c->mem_w8((c->r[3] + (uint32_t)9), (uint8_t)c->r[2]);
  L_8004A264:;
    c->r[31] = 0x8004A26Cu;
    c->r[4] = c->r[16] + c->r[0]; func_80049F80(c);
    { int _t = (c->r[2] == c->r[0]); c->r[4] = (uint32_t)32783u << 16; if (_t) goto L_8004A28C; }
    c->r[4] = c->r[4] + (uint32_t)-12200;
    c->r[3] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)9));
    c->r[2] = c->r[0] + (uint32_t)1;
    c->r[3] = c->r[3] & 253u;
    c->mem_w8((c->r[4] + (uint32_t)9), (uint8_t)c->r[3]); goto L_8004A290;
  L_8004A28C:;
    c->r[2] = c->r[0] + c->r[0];
  L_8004A290:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_8004A2A0(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[31]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)6));
    { int _t = (c->r[3] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_8004A2D0; }
    { int _t = (c->r[3] == c->r[2]); c->r[2] = c->r[0] + c->r[0]; if (_t) goto L_8004A398; }
     goto L_8004A3C4;
  L_8004A2D0:;
    c->r[2] = (uint32_t)32782u << 16;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)32750));
    { int _t = ((int32_t)c->r[2] <= 0); c->r[4] = c->r[0] + (uint32_t)31; if (_t) goto L_8004A3C0; }
    c->r[31] = 0x8004A2ECu;
    c->r[5] = c->r[0] + (uint32_t)-140; func_800310F4(c);
    c->r[3] = c->r[2] + c->r[0];
    { int _t = (c->r[3] == c->r[0]); c->r[4] = c->r[0] + (uint32_t)40; if (_t) goto L_8004A32C; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)40));
    c->r[2] = c->r[2] | 128u;
    c->mem_w8((c->r[3] + (uint32_t)40), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)46));
    c->mem_w16((c->r[3] + (uint32_t)44), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)50));
    c->mem_w16((c->r[3] + (uint32_t)46), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)54));
    c->mem_w16((c->r[3] + (uint32_t)48), (uint16_t)c->r[2]);
  L_8004A32C:;
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x8004A338u;
    c->r[6] = c->r[5] + c->r[0]; func_80074590(c);
    c->r[2] = (uint32_t)32780u << 16;
    c->r[4] = c->r[2] + (uint32_t)-1936;
    c->r[3] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)174));
    c->r[2] = c->r[0] + (uint32_t)255;
    { int _t = (c->r[3] == c->r[2]);  if (_t) goto L_8004A364; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)13));
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)8);
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[0] + (uint32_t)22; if (_t) goto L_8004A368; }
  L_8004A364:;
    c->r[4] = c->r[0] + (uint32_t)12;
  L_8004A368:;
    c->r[31] = 0x8004A370u;
    c->r[5] = c->r[0] + (uint32_t)65; func_8004ED94(c);
    c->r[3] = (uint32_t)32783u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)6));
    c->r[3] = c->r[3] + (uint32_t)-12200;
    c->mem_w8((c->r[16] + (uint32_t)7), (uint8_t)c->r[0]);
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w8((c->r[16] + (uint32_t)6), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)9));
    c->r[2] = c->r[2] | 2u;
    c->mem_w8((c->r[3] + (uint32_t)9), (uint8_t)c->r[2]);
  L_8004A398:;
    c->r[31] = 0x8004A3A0u;
    c->r[4] = c->r[16] + c->r[0]; func_80049F80(c);
    { int _t = (c->r[2] == c->r[0]); c->r[4] = (uint32_t)32783u << 16; if (_t) goto L_8004A3C0; }
    c->r[4] = c->r[4] + (uint32_t)-12200;
    c->r[3] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)9));
    c->r[2] = c->r[0] + (uint32_t)1;
    c->r[3] = c->r[3] & 253u;
    c->mem_w8((c->r[4] + (uint32_t)9), (uint8_t)c->r[3]); goto L_8004A3C4;
  L_8004A3C0:;
    c->r[2] = c->r[0] + c->r[0];
  L_8004A3C4:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_8004B428(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->r[4] = c->r[0] + (uint32_t)31;
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[31]);
    c->r[31] = 0x8004B444u;
    c->r[5] = c->r[0] + (uint32_t)-140; func_800310F4(c);
    c->r[3] = c->r[2] + c->r[0];
    { int _t = (c->r[3] == c->r[0]); c->r[4] = c->r[0] + (uint32_t)40; if (_t) goto L_8004B484; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)40));
    c->r[2] = c->r[2] | 128u;
    c->mem_w8((c->r[3] + (uint32_t)40), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)46));
    c->mem_w16((c->r[3] + (uint32_t)44), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)50));
    c->mem_w16((c->r[3] + (uint32_t)46), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)54));
    c->mem_w16((c->r[3] + (uint32_t)48), (uint16_t)c->r[2]);
  L_8004B484:;
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x8004B490u;
    c->r[6] = c->r[5] + c->r[0]; func_80074590(c);
    c->r[3] = (uint32_t)32780u << 16;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[2] = c->r[0] + (uint32_t)1;
    c->mem_w8((c->r[3] + (uint32_t)-2016), (uint8_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)1;
    c->r[29] = c->r[29] + (uint32_t)24; return;
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[31]);
    c->r[31] = 0x8004B4C8u;
    c->r[5] = c->r[0] + (uint32_t)1; func_8004D4C4(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[2] = (uint32_t)32780u << 16;
    c->mem_w8((c->r[2] + (uint32_t)-1908), (uint8_t)c->r[16]);
    c->r[2] = (uint32_t)32782u << 16;
    c->mem_w8((c->r[2] + (uint32_t)32492), (uint8_t)c->r[16]);
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[5] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[31]);
    c->r[31] = 0x8004B500u;
    c->r[5] = c->r[0] + (uint32_t)1; func_8004D4C4(c);
    c->r[4] = (uint32_t)32782u << 16;
    c->r[4] = c->r[4] + (uint32_t)32384;
    c->r[2] = (uint32_t)32780u << 16;
    c->mem_w8((c->r[2] + (uint32_t)-1907), (uint8_t)c->r[16]);
    c->r[31] = 0x8004B518u;
    c->mem_w8((c->r[4] + (uint32_t)109), (uint8_t)c->r[16]); func_80067DA8(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_8004C0E4(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-40;
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[20]);
    c->r[20] = c->r[4] + c->r[0];
    c->r[3] = (uint32_t)32778u << 16;
    c->r[2] = (uint32_t)32780u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-1936));
    c->r[3] = c->r[3] + (uint32_t)16092;
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[17] = c->r[5] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)36), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[19]);
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[2] = c->r[2] << 2;
    c->r[2] = c->r[2] + c->r[3];
    c->r[16] = c->mem_r32((c->r[2] + (uint32_t)0));
    c->r[2] = c->r[17] & 255u;
    c->r[2] = c->r[2] << 2;
    c->r[16] = c->r[16] + c->r[2];
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)0));
    c->r[19] = c->r[2] & 128u;
    { int _t = (c->r[19] == c->r[0]); c->r[18] = c->r[6] + c->r[0]; if (_t) goto L_8004C158; }
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)2));
    c->r[31] = 0x8004C150u;
    c->r[5] = c->r[0] + (uint32_t)1; func_8004D868(c);
     goto L_8004C164;
  L_8004C158:;
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)2));
    c->r[31] = 0x8004C164u;
    c->r[5] = c->r[0] + (uint32_t)1; func_8004D7EC(c);
  L_8004C164:;
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + c->r[0]; if (_t) goto L_8004C218; }
    { int _t = (c->r[18] != c->r[0]); c->r[4] = c->r[20] + c->r[0]; if (_t) goto L_8004C17C; }
    c->r[5] = c->r[0] + c->r[0]; goto L_8004C180;
  L_8004C17C:;
    c->r[5] = c->r[0] + (uint32_t)1;
  L_8004C180:;
    c->r[6] = c->r[0] + (uint32_t)5;
    c->r[31] = 0x8004C18Cu;
    c->r[7] = c->r[0] + (uint32_t)4; func_80072DDC(c);
    c->r[4] = c->r[2] + c->r[0];
    { int _t = (c->r[4] != c->r[0]); c->r[2] = (uint32_t)32773u << 16; if (_t) goto L_8004C1A0; }
    c->r[2] = c->r[0] + c->r[0]; goto L_8004C218;
  L_8004C1A0:;
    c->r[2] = c->r[2] + (uint32_t)-15816;
    c->mem_w32((c->r[4] + (uint32_t)28), c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)1));
    c->mem_w8((c->r[4] + (uint32_t)3), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)0));
    c->r[3] = c->r[0] + (uint32_t)1;
    c->mem_w16((c->r[4] + (uint32_t)96), (uint16_t)c->r[3]);
    c->r[2] = c->r[2] & 127u;
    c->mem_w8((c->r[4] + (uint32_t)94), (uint8_t)c->r[2]);
    c->r[3] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)2));
    c->r[2] = c->r[0] + (uint32_t)2;
    c->mem_w16((c->r[4] + (uint32_t)100), (uint16_t)c->r[2]);
    { int _t = (c->r[19] == c->r[0]); c->mem_w16((c->r[4] + (uint32_t)98), (uint16_t)c->r[3]); if (_t) goto L_8004C1E4; }
    c->r[2] = c->r[0] + (uint32_t)6;
    c->mem_w16((c->r[4] + (uint32_t)100), (uint16_t)c->r[2]);
  L_8004C1E4:;
    c->r[2] = (uint32_t)32780u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-1936));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[4] + c->r[0]; if (_t) goto L_8004C218; }
    c->r[2] = c->r[17] + (uint32_t)-1;
    c->r[2] = c->r[2] & 255u;
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)5);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[17] & 255u; if (_t) goto L_8004C214; }
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->mem_w16((c->r[4] + (uint32_t)64), (uint16_t)c->r[2]);
  L_8004C214:;
    c->r[2] = c->r[4] + c->r[0];
  L_8004C218:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)36));
    c->r[20] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)40; return;
    return;
}

static void leaf_8004CBD8(Core* c) {
    c->r[2] = (uint32_t)32780u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-1936));
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    { int _t = (c->r[2] != c->r[0]); c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]); if (_t) goto L_8004CC44; }
    c->r[2] = (uint32_t)32780u << 16;
    c->r[17] = c->r[2] + (uint32_t)-2040;
    c->r[5] = c->r[5] & 255u;
    c->r[3] = c->r[0] + (uint32_t)1;
    c->r[2] = c->mem_r32((c->r[17] + (uint32_t)72));
    c->r[16] = c->r[3] << (c->r[5] & 31);
    c->r[2] = c->r[2] & c->r[16];
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + c->r[0]; if (_t) goto L_8004CC50; }
    c->r[31] = 0x8004CC20u;
    c->r[6] = c->r[0] + c->r[0]; func_8004C0E4(c);
    c->r[3] = c->r[2] + c->r[0];
    { int _t = (c->r[3] == c->r[0]);  if (_t) goto L_8004CC3C; }
    c->r[2] = c->mem_r32((c->r[17] + (uint32_t)72));
    c->r[2] = c->r[2] | c->r[16];
    c->mem_w32((c->r[17] + (uint32_t)72), c->r[2]);
  L_8004CC3C:;
    c->r[2] = c->r[3] + c->r[0]; goto L_8004CC50;
  L_8004CC44:;
    c->r[5] = c->r[5] & 255u;
    c->r[31] = 0x8004CC50u;
    c->r[6] = c->r[0] + c->r[0]; func_8004C0E4(c);
  L_8004CC50:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

static void leaf_8004D514(Core* c) {
    c->r[2] = (uint32_t)32780u << 16;
    c->r[5] = c->r[2] + (uint32_t)-1936;
    c->r[3] = c->r[4] + c->r[5];
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)580));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)32778u << 16; if (_t) goto L_8004D5A8; }
    c->r[8] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)836));
    c->mem_w8((c->r[3] + (uint32_t)836), (uint8_t)c->r[0]);
    c->r[3] = c->r[2] + (uint32_t)11240;
    c->r[2] = c->r[4] << 1;
    c->r[2] = c->r[2] + c->r[4];
    c->r[2] = c->r[2] << 2;
    c->r[2] = c->r[2] + c->r[3];
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)0));
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[0] + c->r[0]; if (_t) goto L_8004D5B0; }
    c->r[7] = c->r[5] + c->r[0];
    c->r[2] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)50));
    c->r[6] = c->r[3] + c->r[0];
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->mem_w8((c->r[7] + (uint32_t)50), (uint8_t)c->r[2]);
  L_8004D570:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[6] + (uint32_t)0));
    { int _t = (c->r[2] != c->r[0]); c->r[5] = c->r[4] + c->r[7]; if (_t) goto L_8004D598; }
    c->r[3] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)836));
    c->r[2] = (uint32_t)((int32_t)c->r[8] < (int32_t)c->r[3]);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[3] + (uint32_t)-1; if (_t) goto L_8004D598; }
    c->mem_w8((c->r[5] + (uint32_t)836), (uint8_t)c->r[2]);
  L_8004D598:;
    c->r[4] = c->r[4] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[4] < 256);
    { int _t = (c->r[2] != c->r[0]); c->r[6] = c->r[6] + (uint32_t)12; if (_t) goto L_8004D570; }
  L_8004D5A8:;
     return;
  L_8004D5B0:;
    c->r[7] = c->r[5] + c->r[0];
    c->r[2] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)49));
    c->r[6] = c->r[3] + c->r[0];
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->mem_w8((c->r[7] + (uint32_t)49), (uint8_t)c->r[2]);
  L_8004D5C4:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[6] + (uint32_t)0));
    { int _t = (c->r[2] == c->r[0]); c->r[5] = c->r[4] + c->r[7]; if (_t) goto L_8004D5EC; }
    c->r[3] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)836));
    c->r[2] = (uint32_t)((int32_t)c->r[8] < (int32_t)c->r[3]);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[3] + (uint32_t)-1; if (_t) goto L_8004D5EC; }
    c->mem_w8((c->r[5] + (uint32_t)836), (uint8_t)c->r[2]);
  L_8004D5EC:;
    c->r[4] = c->r[4] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[4] < 256);
    { int _t = (c->r[2] != c->r[0]); c->r[6] = c->r[6] + (uint32_t)12; if (_t) goto L_8004D5C4; }
     return;
    return;
}

static void leaf_8004D650(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->r[3] = (uint32_t)32780u << 16;
    c->r[3] = c->r[3] + (uint32_t)-1936;
    c->r[3] = c->r[4] + c->r[3];
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[31]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)580));
    c->r[2] = c->r[2] - c->r[5];
    c->r[31] = 0x8004D678u;
    c->mem_w8((c->r[3] + (uint32_t)580), (uint8_t)c->r[2]); func_8004D514(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_8004D714(Core* c) {
    c->r[2] = c->r[4] + c->r[0];
    { int _t = ((int32_t)c->r[4] >= 0); c->r[3] = c->r[5] + c->r[0]; if (_t) goto L_8004D724; }
    c->r[2] = c->r[4] + (uint32_t)7;
  L_8004D724:;
    c->r[5] = (uint32_t)((int32_t)c->r[2] >> 3);
    c->r[2] = c->r[5] << 3;
    c->r[4] = c->r[4] - c->r[2];
    c->r[2] = c->r[3] & 255u;
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)32780u << 16; if (_t) goto L_8004D76C; }
    c->r[2] = c->r[2] + (uint32_t)-1936;
    c->r[5] = c->r[5] << 16;
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16);
    c->r[5] = c->r[5] + c->r[2];
    c->r[4] = c->r[4] << 16;
    c->r[4] = (uint32_t)((int32_t)c->r[4] >> 16);
    c->r[2] = c->r[0] + (uint32_t)1;
    c->r[3] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)1092));
    c->r[2] = c->r[2] << (c->r[4] & 31);
    c->r[3] = c->r[3] | c->r[2];
    c->mem_w8((c->r[5] + (uint32_t)1092), (uint8_t)c->r[3]); return;
  L_8004D76C:;
    c->r[2] = c->r[2] + (uint32_t)-1936;
    c->r[5] = c->r[5] << 16;
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16);
    c->r[5] = c->r[5] + c->r[2];
    c->r[4] = c->r[4] << 16;
    c->r[4] = (uint32_t)((int32_t)c->r[4] >> 16);
    c->r[2] = c->r[0] + (uint32_t)1;
    c->r[3] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)1220));
    c->r[2] = c->r[2] << (c->r[4] & 31);
    c->r[3] = c->r[3] | c->r[2];
    c->mem_w8((c->r[5] + (uint32_t)1220), (uint8_t)c->r[3]); return;
    return;
}

static void leaf_8004D79C(Core* c) {
    c->r[2] = c->r[4] + c->r[0];
    { int _t = ((int32_t)c->r[2] >= 0); c->r[5] = c->r[2] + c->r[0]; if (_t) goto L_8004D7AC; }
    c->r[5] = c->r[2] + (uint32_t)7;
  L_8004D7AC:;
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 3);
    c->r[4] = c->r[5] << 3;
    c->r[4] = c->r[2] - c->r[4];
    c->r[2] = (uint32_t)32780u << 16;
    c->r[2] = c->r[2] + (uint32_t)-1936;
    c->r[5] = c->r[5] << 16;
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16);
    c->r[5] = c->r[5] + c->r[2];
    c->r[4] = c->r[4] << 16;
    c->r[4] = (uint32_t)((int32_t)c->r[4] >> 16);
    c->r[2] = c->r[0] + (uint32_t)1;
    c->r[3] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)1348));
    c->r[2] = c->r[2] << (c->r[4] & 31);
    c->r[3] = c->r[3] | c->r[2];
    c->mem_w8((c->r[5] + (uint32_t)1348), (uint8_t)c->r[3]); return;
    return;
}

static void leaf_8004D8B0(Core* c) {
    c->r[3] = c->r[0] + (uint32_t)127;
    c->r[2] = (uint32_t)32780u << 16;
    c->r[2] = c->r[2] + (uint32_t)-1936;
    c->r[2] = c->r[2] + c->r[3];
  L_8004D8C0:;
    c->mem_w8((c->r[2] + (uint32_t)1348), (uint8_t)c->r[0]);
    c->r[3] = c->r[3] + (uint32_t)-1;
    { int _t = ((int32_t)c->r[3] >= 0); c->r[2] = c->r[2] + (uint32_t)-1; if (_t) goto L_8004D8C0; }
     return;
    return;
}

static void leaf_8004DAEC(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-40;
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[19]);
    c->r[19] = c->r[4] + c->r[0];
    c->r[5] = (uint32_t)8064u << 16;
    c->r[2] = c->r[0] + (uint32_t)20;
    c->mem_w16((c->r[5] + (uint32_t)192), (uint16_t)c->r[2]);
    c->r[5] = c->r[5] + (uint32_t)192;
    c->r[2] = (uint32_t)32782u << 16;
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->mem_w16((c->r[5] + (uint32_t)2), (uint16_t)c->r[0]);
    c->r[18] = c->mem_r32((c->r[2] + (uint32_t)32592));
    c->r[2] = c->r[0] + (uint32_t)50;
    c->r[17] = (uint32_t)8064u << 16;
    c->r[16] = c->r[17] + (uint32_t)200;
    c->r[6] = c->r[16] + c->r[0];
    c->mem_w16((c->r[5] + (uint32_t)4), (uint16_t)c->r[2]);
    c->r[31] = 0x8004DB40u;
    c->r[4] = c->r[18] + (uint32_t)24; func_800844C0(c);
    c->r[2] = (uint32_t)c->mem_r16((c->r[17] + (uint32_t)200));
    c->r[3] = (uint32_t)c->mem_r16((c->r[18] + (uint32_t)44));
    c->r[2] = c->r[2] + c->r[3];
    c->mem_w16((c->r[19] + (uint32_t)46), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)2));
    c->r[3] = (uint32_t)c->mem_r16((c->r[18] + (uint32_t)48));
    c->r[2] = c->r[2] + c->r[3];
    c->mem_w16((c->r[19] + (uint32_t)50), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)4));
    c->r[3] = (uint32_t)c->mem_r16((c->r[18] + (uint32_t)52));
    c->r[4] = c->r[19] + c->r[0];
    c->r[2] = c->r[2] + c->r[3];
    c->r[31] = 0x8004DB80u;
    c->mem_w16((c->r[19] + (uint32_t)54), (uint16_t)c->r[2]); func_8007778C(c);
    c->r[31] = 0x8004DB88u;
    c->r[4] = c->r[19] + c->r[0]; func_80077B5C(c);
    c->r[2] = (uint32_t)32780u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-2038));
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)1);
    c->r[29] = c->r[29] + (uint32_t)40; return;
    c->r[2] = (uint32_t)32780u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-2038));
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)1); return;
    return;
}

static void leaf_8004ED0C(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->r[2] = c->r[5] | 32768u;
    c->mem_w16((c->r[29] + (uint32_t)18), (uint16_t)c->r[2]);
    c->r[2] = c->r[0] | 65534u;
    c->r[5] = c->r[5] << 16;
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16);
    c->mem_w16((c->r[29] + (uint32_t)20), (uint16_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)1;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[31]);
    { int _t = (c->r[5] == c->r[2]); c->mem_w16((c->r[29] + (uint32_t)16), (uint16_t)c->r[4]); if (_t) goto L_8004ED74; }
    c->r[2] = (uint32_t)((int32_t)c->r[5] < 2);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)2; if (_t) goto L_8004ED54; }
    { int _t = (c->r[5] == c->r[0]); c->r[6] = c->r[0] + (uint32_t)65; if (_t) goto L_8004ED64; }
    c->r[4] = c->r[29] + (uint32_t)16; goto L_8004ED7C;
  L_8004ED54:;
    { int _t = (c->r[5] == c->r[2]); c->r[2] = c->r[0] + (uint32_t)3; if (_t) goto L_8004ED6C; }
    { int _t = (c->r[5] != c->r[2]); c->r[6] = c->r[0] + (uint32_t)65; if (_t) goto L_8004ED78; }
  L_8004ED64:;
    c->r[6] = c->r[0] + (uint32_t)64; goto L_8004ED78;
  L_8004ED6C:;
    c->r[6] = c->r[0] + (uint32_t)66; goto L_8004ED78;
  L_8004ED74:;
    c->r[6] = c->r[0] + (uint32_t)65;
  L_8004ED78:;
    c->r[4] = c->r[29] + (uint32_t)16;
  L_8004ED7C:;
    c->r[31] = 0x8004ED84u;
    c->r[5] = c->r[0] + (uint32_t)1; func_8004FA38(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

static void leaf_8004EE88(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->r[8] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[31]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[8] + (uint32_t)8));
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->mem_w16((c->r[8] + (uint32_t)8), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    { int _t = ((int32_t)c->r[2] <= 0); c->r[5] = c->r[0] + c->r[0]; if (_t) goto L_8004EF24; }
    c->r[7] = c->r[0] + (uint32_t)140;
    c->r[6] = c->r[8] + c->r[0];
  L_8004EEB8:;
    c->r[4] = c->r[6] + (uint32_t)12;
    c->r[2] = c->r[8] + c->r[7];
    c->r[3] = c->r[2] + (uint32_t)12;
    c->r[2] = c->r[2] + (uint32_t)140;
  L_8004EEC8:;
    c->r[9] = c->mem_r32((c->r[3] + (uint32_t)0));
    c->r[10] = c->mem_r32((c->r[3] + (uint32_t)4));
    c->r[11] = c->mem_r32((c->r[3] + (uint32_t)8));
    c->r[12] = c->mem_r32((c->r[3] + (uint32_t)12));
    c->mem_w32((c->r[4] + (uint32_t)0), c->r[9]);
    c->mem_w32((c->r[4] + (uint32_t)4), c->r[10]);
    c->mem_w32((c->r[4] + (uint32_t)8), c->r[11]);
    c->mem_w32((c->r[4] + (uint32_t)12), c->r[12]);
    c->r[3] = c->r[3] + (uint32_t)16;
    { int _t = (c->r[3] != c->r[2]); c->r[4] = c->r[4] + (uint32_t)16; if (_t) goto L_8004EEC8; }
    c->r[7] = c->r[7] + (uint32_t)140;
    c->r[9] = c->mem_r32((c->r[3] + (uint32_t)0));
    c->r[10] = c->mem_r32((c->r[3] + (uint32_t)4));
    c->r[11] = c->mem_r32((c->r[3] + (uint32_t)8));
    c->mem_w32((c->r[4] + (uint32_t)0), c->r[9]);
    c->mem_w32((c->r[4] + (uint32_t)4), c->r[10]);
    c->mem_w32((c->r[4] + (uint32_t)8), c->r[11]);
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[8] + (uint32_t)8));
    c->r[5] = c->r[5] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[5] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]); c->r[6] = c->r[6] + (uint32_t)140; if (_t) goto L_8004EEB8; }
  L_8004EF24:;
    c->r[4] = c->r[5] << 3;
    c->r[4] = c->r[4] + c->r[5];
    c->r[4] = c->r[4] << 2;
    c->r[4] = c->r[4] - c->r[5];
    c->r[4] = c->r[4] << 2;
    c->r[4] = c->r[4] + (uint32_t)12;
    c->r[31] = 0x8004EF44u;
    c->r[4] = c->r[8] + c->r[4]; func_8004EE50(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_8004EF54(Core* c) {
    c->r[6] = c->r[0] + (uint32_t)250;
    c->r[3] = c->r[0] + (uint32_t)255;
  L_8004EF5C:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)0));
    c->r[5] = c->r[5] + (uint32_t)1;
    c->mem_w8((c->r[4] + (uint32_t)0), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)0));
    { int _t = (c->r[2] == c->r[6]); c->r[4] = c->r[4] + (uint32_t)1; if (_t) goto L_8004EF80; }
    { int _t = (c->r[2] != c->r[3]);  if (_t) goto L_8004EF5C; }
  L_8004EF80:;
    c->mem_w8((c->r[4] + (uint32_t)0), (uint8_t)c->r[3]);
    c->r[2] = c->r[4] + c->r[0]; return;
    return;
}

static void leaf_8004EF8C(Core* c) {
    { int _t = (c->r[5] == c->r[0]); c->r[2] = c->r[4] + c->r[0]; if (_t) goto L_8004EFB8; }
  L_8004EF94:;
    c->r[3] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)0));
    c->r[2] = c->r[0] + (uint32_t)250;
    { int _t = (c->r[3] != c->r[2]);  if (_t) goto L_8004EFB0; }
    c->r[5] = c->r[5] + (uint32_t)-1;
    { int _t = (c->r[5] == c->r[0]); c->r[2] = c->r[4] + (uint32_t)1; if (_t) goto L_8004EFB8; }
  L_8004EFB0:;
    c->r[4] = c->r[4] + (uint32_t)1; goto L_8004EF94;
  L_8004EFB8:;
     return;
    return;
}

static void leaf_8004F058(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-40;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[5] + c->r[0];
    c->r[3] = c->r[6] + c->r[0];
    c->r[2] = (uint32_t)32780u << 16;
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[17] = c->r[2] + (uint32_t)-2744;
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[19]);
    c->r[19] = c->r[4] + c->r[0];
    c->r[6] = c->r[6] << 16;
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[31]);
    { int _t = (c->r[6] == c->r[0]); c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]); if (_t) goto L_8004F0C4; }
    c->r[6] = c->r[0] | 65534u;
    c->r[5] = c->r[0] | 65535u;
  L_8004F094:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)0));
    { int _t = (c->r[2] != c->r[6]);  if (_t) goto L_8004F0AC; }
    c->r[2] = c->r[0] + c->r[0]; goto L_8004F168;
  L_8004F0AC:;
    { int _t = (c->r[2] != c->r[5]); c->r[2] = c->r[3] << 16; if (_t) goto L_8004F0BC; }
    c->r[3] = c->r[3] + (uint32_t)-1;
    c->r[2] = c->r[3] << 16;
  L_8004F0BC:;
    { int _t = (c->r[2] != c->r[0]); c->r[16] = c->r[16] + (uint32_t)2; if (_t) goto L_8004F094; }
  L_8004F0C4:;
    c->r[3] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)0));
    c->r[2] = c->r[3] + (uint32_t)2;
    c->r[2] = c->r[2] & 65535u;
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)2);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)32778u << 16; if (_t) goto L_8004F154; }
    c->r[18] = c->r[2] + (uint32_t)11240;
    c->r[2] = c->r[3] & 32768u;
  L_8004F0E8:;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[3] & 4095u; if (_t) goto L_8004F114; }
    c->r[2] = c->r[2] << 1;
    c->r[3] = c->mem_r32((c->r[17] + (uint32_t)692));
    c->r[5] = c->mem_r32((c->r[17] + (uint32_t)696));
    c->r[2] = c->r[2] + c->r[3];
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)0));
    c->r[31] = 0x8004F10Cu;
    c->r[5] = c->r[5] + c->r[2]; func_8004EF54(c);
    c->r[4] = c->r[2] + c->r[0]; goto L_8004F134;
  L_8004F114:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)0));
    c->r[5] = c->r[2] << 1;
    c->r[5] = c->r[5] + c->r[2];
    c->r[5] = c->r[5] << 2;
    c->r[31] = 0x8004F130u;
    c->r[5] = c->r[5] + c->r[18]; func_8004EAD0(c);
    c->r[4] = c->r[2] + c->r[0];
  L_8004F134:;
    c->r[16] = c->r[16] + (uint32_t)2;
    c->r[3] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)0));
    c->r[2] = c->r[3] + (uint32_t)2;
    c->r[2] = c->r[2] & 65535u;
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)2);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[3] & 32768u; if (_t) goto L_8004F0E8; }
  L_8004F154:;
    c->r[4] = c->r[19] + c->r[0];
    c->r[31] = 0x8004F160u;
    c->r[5] = c->r[0] + c->r[0]; func_8004EA4C(c);
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
  L_8004F168:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)40; return;
    return;
}

static void leaf_8004F378(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-64;
    c->mem_w32((c->r[29] + (uint32_t)52), c->r[17]);
    c->r[17] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)56), c->r[31]);
    c->r[31] = 0x8004F390u;
    c->mem_w32((c->r[29] + (uint32_t)48), c->r[16]); func_80077D64(c);
    c->r[16] = c->r[17] + (uint32_t)432;
    c->r[4] = c->r[17] + c->r[0];
    c->r[31] = 0x8004F3A0u;
    c->r[5] = c->r[16] + c->r[0]; func_8004F184(c);
    c->r[2] = (uint32_t)c->mem_r16((c->r[17] + (uint32_t)10));
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->mem_w16((c->r[17] + (uint32_t)10), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    { int _t = ((int32_t)c->r[2] <= 0); c->r[3] = c->r[0] + c->r[0]; if (_t) goto L_8004F414; }
  L_8004F3BC:;
    c->r[3] = c->r[3] + (uint32_t)1;
    c->r[6] = c->mem_r32((c->r[16] + (uint32_t)32));
    c->r[7] = c->mem_r32((c->r[16] + (uint32_t)36));
    c->r[8] = c->mem_r32((c->r[16] + (uint32_t)40));
    c->r[9] = c->mem_r32((c->r[16] + (uint32_t)44));
    c->mem_w32((c->r[16] + (uint32_t)0), c->r[6]);
    c->mem_w32((c->r[16] + (uint32_t)4), c->r[7]);
    c->mem_w32((c->r[16] + (uint32_t)8), c->r[8]);
    c->mem_w32((c->r[16] + (uint32_t)12), c->r[9]);
    c->r[6] = c->mem_r32((c->r[16] + (uint32_t)48));
    c->r[7] = c->mem_r32((c->r[16] + (uint32_t)52));
    c->r[8] = c->mem_r32((c->r[16] + (uint32_t)56));
    c->r[9] = c->mem_r32((c->r[16] + (uint32_t)60));
    c->mem_w32((c->r[16] + (uint32_t)16), c->r[6]);
    c->mem_w32((c->r[16] + (uint32_t)20), c->r[7]);
    c->mem_w32((c->r[16] + (uint32_t)24), c->r[8]);
    c->mem_w32((c->r[16] + (uint32_t)28), c->r[9]);
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[17] + (uint32_t)10));
    c->r[2] = (uint32_t)((int32_t)c->r[3] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]); c->r[16] = c->r[16] + (uint32_t)32; if (_t) goto L_8004F3BC; }
  L_8004F414:;
    c->r[31] = 0x8004F41Cu;
    c->r[4] = c->r[16] + c->r[0]; func_8004EE2C(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)56));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)52));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)48));
    c->r[29] = c->r[29] + (uint32_t)64; return;
    return;
}

static void leaf_8004F430(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[31]);
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)10));
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_8004F464; }
    c->r[31] = 0x8004F458u;
     func_8004F378(c);
    c->r[2] = c->r[0] + (uint32_t)1;
    c->mem_w16((c->r[16] + (uint32_t)4), (uint16_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)1), (uint8_t)c->r[2]);
  L_8004F464:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_8004F474(Core* c) {
    c->r[6] = c->r[4] + (uint32_t)12;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)8));
    { int _t = ((int32_t)c->r[2] <= 0); c->r[3] = c->r[0] + c->r[0]; if (_t) goto L_8004F4B4; }
    c->r[5] = c->r[4] + (uint32_t)14;
  L_8004F48C:;
    c->r[3] = c->r[3] + (uint32_t)1;
    c->r[2] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)0));
    c->r[6] = c->r[6] + (uint32_t)140;
    c->r[2] = c->r[2] + (uint32_t)2;
    c->mem_w16((c->r[5] + (uint32_t)0), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)8));
    c->r[2] = (uint32_t)((int32_t)c->r[3] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]); c->r[5] = c->r[5] + (uint32_t)140; if (_t) goto L_8004F48C; }
  L_8004F4B4:;
    c->r[6] = c->r[6] + (uint32_t)-140;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[6] + (uint32_t)2));
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 158);
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_8004F50C; }
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)8));
    { int _t = ((int32_t)c->r[2] <= 0); c->r[3] = c->r[0] + c->r[0]; if (_t) goto L_8004F500; }
    c->r[5] = c->r[0] + (uint32_t)158;
  L_8004F4E0:;
    c->mem_w16((c->r[6] + (uint32_t)2), (uint16_t)c->r[5]);
    c->r[5] = c->r[5] + (uint32_t)18;
    c->r[3] = c->r[3] + (uint32_t)1;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)8));
    c->r[2] = (uint32_t)((int32_t)c->r[3] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]); c->r[6] = c->r[6] + (uint32_t)-140; if (_t) goto L_8004F4E0; }
  L_8004F500:;
    c->r[2] = c->r[0] + (uint32_t)2;
    c->mem_w16((c->r[4] + (uint32_t)4), (uint16_t)c->r[0]);
    c->mem_w8((c->r[4] + (uint32_t)1), (uint8_t)c->r[2]);
  L_8004F50C:;
     return;
    return;
}

static void leaf_8004F514(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-72;
    c->mem_w32((c->r[29] + (uint32_t)60), c->r[19]);
    c->r[19] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)64), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)56), c->r[18]);
    c->mem_w32((c->r[29] + (uint32_t)52), c->r[17]);
    c->mem_w32((c->r[29] + (uint32_t)48), c->r[16]);
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[19] + (uint32_t)10));
    { int _t = (c->r[3] == c->r[0]); c->r[2] = (uint32_t)((int32_t)c->r[3] < 4); if (_t) goto L_8004F55C; }
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[0] + (uint32_t)1; if (_t) goto L_8004F574; }
    c->r[2] = (uint32_t)((int32_t)c->r[3] < 2);
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[0] + (uint32_t)20; if (_t) goto L_8004F574; }
    c->r[4] = c->r[0] + (uint32_t)10; goto L_8004F574;
  L_8004F55C:;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)311));
    c->r[2] = c->r[0] + (uint32_t)1;
    { int _t = (c->r[3] != c->r[2]); c->r[4] = c->r[0] + (uint32_t)30; if (_t) goto L_8004F574; }
    c->r[4] = c->r[0] + (uint32_t)16;
  L_8004F574:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[19] + (uint32_t)4));
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w16((c->r[19] + (uint32_t)4), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < (int32_t)c->r[4]);
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_8004F6B4; }
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[19] + (uint32_t)8));
    { int _t = ((int32_t)c->r[2] <= 0); c->r[18] = c->r[0] + c->r[0]; if (_t) goto L_8004F6A4; }
    c->r[16] = c->r[19] + (uint32_t)23;
    c->r[17] = c->r[19] + (uint32_t)73;
  L_8004F5B0:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)-3));
    c->r[2] = c->r[2] + (uint32_t)1;
    c->r[3] = c->r[2] & 255u;
    c->mem_w8((c->r[16] + (uint32_t)-3), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)(c->r[3] < (uint32_t)3);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)2; if (_t) goto L_8004F5E8; }
    c->r[3] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)-7));
    c->mem_w8((c->r[16] + (uint32_t)-1), (uint8_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)1024;
    c->mem_w16((c->r[16] + (uint32_t)3), (uint16_t)c->r[2]);
    c->mem_w16((c->r[16] + (uint32_t)1), (uint16_t)c->r[3]); goto L_8004F688;
  L_8004F5E8:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)-2));
    c->r[2] = (uint32_t)(c->r[3] < c->r[2]);
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_8004F688; }
    c->r[7] = c->mem_r32((c->r[16] + (uint32_t)97));
    c->r[8] = c->mem_r32((c->r[16] + (uint32_t)101));
    c->r[9] = c->mem_r32((c->r[16] + (uint32_t)105));
    c->r[10] = c->mem_r32((c->r[16] + (uint32_t)109));
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[7]);
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[8]);
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[9]);
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[10]);
    c->r[7] = c->mem_r32((c->r[16] + (uint32_t)113));
    c->r[8] = c->mem_r32((c->r[16] + (uint32_t)117));
    c->r[9] = c->mem_r32((c->r[16] + (uint32_t)121));
    c->r[10] = c->mem_r32((c->r[16] + (uint32_t)125));
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[7]);
    c->mem_w32((c->r[29] + (uint32_t)36), c->r[8]);
    c->mem_w32((c->r[29] + (uint32_t)40), c->r[9]);
    c->mem_w32((c->r[29] + (uint32_t)44), c->r[10]);
    c->r[4] = c->mem_r32((c->r[29] + (uint32_t)16));
    { int _t = (c->r[4] != c->r[0]);  if (_t) goto L_8004F664; }
    c->r[4] = c->r[17] + c->r[0];
    c->r[6] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)-3));
    c->r[31] = 0x8004F65Cu;
    c->r[5] = c->r[29] + (uint32_t)20; func_8004F058(c);
    c->r[2] = c->r[0] + (uint32_t)1; goto L_8004F680;
  L_8004F664:;
    c->r[5] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)-3));
    c->r[31] = 0x8004F670u;
     func_8004EF8C(c);
    c->r[4] = c->r[17] + c->r[0];
    c->r[31] = 0x8004F67Cu;
    c->r[5] = c->r[2] + c->r[0]; func_8004EF54(c);
    c->r[2] = c->r[0] + (uint32_t)1;
  L_8004F680:;
    c->mem_w8((c->r[16] + (uint32_t)-1), (uint8_t)c->r[2]);
    c->mem_w8((c->r[16] + (uint32_t)0), (uint8_t)c->r[0]);
  L_8004F688:;
    c->r[18] = c->r[18] + (uint32_t)1;
    c->r[16] = c->r[16] + (uint32_t)140;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[19] + (uint32_t)8));
    c->r[2] = (uint32_t)((int32_t)c->r[18] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]); c->r[17] = c->r[17] + (uint32_t)140; if (_t) goto L_8004F5B0; }
  L_8004F6A4:;
    c->r[2] = c->r[0] + (uint32_t)8;
    c->mem_w16((c->r[19] + (uint32_t)4), (uint16_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)3;
    c->mem_w8((c->r[19] + (uint32_t)1), (uint8_t)c->r[2]);
  L_8004F6B4:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)64));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)60));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)56));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)52));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)48));
    c->r[29] = c->r[29] + (uint32_t)72; return;
    return;
}

static void leaf_8004F6D0(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-48;
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[19]);
    c->r[19] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[17] = c->r[0] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
    c->r[18] = c->r[19] + (uint32_t)12;
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[20]);
    c->r[20] = c->r[17] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)40), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)36), c->r[21]);
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[19] + (uint32_t)8));
    { int _t = ((int32_t)c->r[2] <= 0); c->r[21] = c->r[17] + c->r[0]; if (_t) goto L_8004F860; }
    c->r[16] = c->r[19] + (uint32_t)16;
  L_8004F714:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)4));
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)3);
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_8004F72C; }
    c->r[21] = c->r[21] + (uint32_t)1;
  L_8004F72C:;
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)6));
    c->r[2] = c->r[0] + (uint32_t)1;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = (uint32_t)((int32_t)c->r[3] < 2); if (_t) goto L_8004F76C; }
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)2; if (_t) goto L_8004F754; }
    { int _t = (c->r[3] == c->r[0]);  if (_t) goto L_8004F764; }
    c->r[20] = c->r[20] + (uint32_t)1; goto L_8004F844;
  L_8004F754:;
    { int _t = (c->r[3] == c->r[2]);  if (_t) goto L_8004F7C8; }
    c->r[20] = c->r[20] + (uint32_t)1; goto L_8004F844;
  L_8004F764:;
    c->r[17] = c->r[17] + (uint32_t)1; goto L_8004F840;
  L_8004F76C:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)7));
    c->r[2] = c->r[2] + (uint32_t)2;
    c->mem_w8((c->r[16] + (uint32_t)7), (uint8_t)c->r[2]);
    c->r[2] = c->r[2] & 255u;
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)8);
    { int _t = (c->r[2] != c->r[0]); c->r[5] = c->r[18] + (uint32_t)16; if (_t) goto L_8004F840; }
    c->r[4] = c->r[18] + (uint32_t)61;
    c->r[3] = c->r[0] + c->r[0];
  L_8004F794:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)0));
    c->r[4] = c->r[4] + (uint32_t)1;
    c->r[3] = c->r[3] + (uint32_t)1;
    c->mem_w8((c->r[5] + (uint32_t)0), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)((int32_t)c->r[3] < 45);
    { int _t = (c->r[2] != c->r[0]); c->r[5] = c->r[5] + (uint32_t)1; if (_t) goto L_8004F794; }
    c->r[17] = c->r[17] + (uint32_t)1;
    c->r[2] = c->r[0] + (uint32_t)255;
    c->mem_w8((c->r[16] + (uint32_t)57), (uint8_t)c->r[2]);
    c->mem_w8((c->r[16] + (uint32_t)7), (uint8_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)6), (uint8_t)c->r[0]); goto L_8004F840;
  L_8004F7C8:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)10));
    c->r[2] = c->r[2] + (uint32_t)-256;
    c->mem_w16((c->r[16] + (uint32_t)10), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    { int _t = ((int32_t)c->r[2] >= 0);  if (_t) goto L_8004F7F0; }
    c->r[17] = c->r[17] + (uint32_t)1;
    c->mem_w16((c->r[16] + (uint32_t)10), (uint16_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)6), (uint8_t)c->r[0]);
  L_8004F7F0:;
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)10));
    c->r[31] = 0x8004F7FCu;
     func_80083E80(c);
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)8));
    { int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[3]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[2] = c->lo;
    { int _t = ((int32_t)c->r[2] >= 0);  if (_t) goto L_8004F818; }
    c->r[2] = c->r[2] + (uint32_t)4095;
  L_8004F818:;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 12);
    c->mem_w16((c->r[16] + (uint32_t)0), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    c->r[3] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = c->r[2] >> 31;
    c->r[3] = c->r[3] + c->r[2];
    c->r[3] = (uint32_t)((int32_t)c->r[3] >> 1);
    c->r[2] = c->r[0] + (uint32_t)160;
    c->r[2] = c->r[2] - c->r[3];
    c->mem_w16((c->r[18] + (uint32_t)0), (uint16_t)c->r[2]);
  L_8004F840:;
    c->r[20] = c->r[20] + (uint32_t)1;
  L_8004F844:;
    c->r[16] = c->r[16] + (uint32_t)140;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[19] + (uint32_t)8));
    c->r[2] = (uint32_t)((int32_t)c->r[20] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]); c->r[18] = c->r[18] + (uint32_t)140; if (_t) goto L_8004F714; }
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[19] + (uint32_t)8));
  L_8004F860:;
    { int _t = (c->r[17] != c->r[2]);  if (_t) goto L_8004F8B8; }
    { int _t = (c->r[21] == c->r[0]);  if (_t) goto L_8004F880; }
    c->r[31] = 0x8004F87Cu;
    c->r[4] = c->r[19] + c->r[0]; func_8004EE88(c);
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[19] + (uint32_t)8));
  L_8004F880:;
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_8004F894; }
    c->mem_w8((c->r[19] + (uint32_t)1), (uint8_t)c->r[0]); goto L_8004F8B8;
  L_8004F894:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[19] + (uint32_t)10));
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)2; if (_t) goto L_8004F8B0; }
    c->r[31] = 0x8004F8ACu;
    c->r[4] = c->r[19] + c->r[0]; func_8004F378(c);
    c->r[2] = c->r[0] + (uint32_t)1;
  L_8004F8B0:;
    c->mem_w16((c->r[19] + (uint32_t)4), (uint16_t)c->r[0]);
    c->mem_w8((c->r[19] + (uint32_t)1), (uint8_t)c->r[2]);
  L_8004F8B8:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)40));
    c->r[21] = c->mem_r32((c->r[29] + (uint32_t)36));
    c->r[20] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)48; return;
    return;
}

static void leaf_80050894(Core* c) {
    c->r[2] = (uint32_t)32783u << 16;
    c->r[2] = c->r[2] + (uint32_t)-32600;
    c->mem_w8((c->r[2] + (uint32_t)16540), (uint8_t)c->r[4]);
    c->mem_w8((c->r[2] + (uint32_t)8236), (uint8_t)c->r[4]); return;
    return;
}

static void leaf_800521F4(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->r[2] = (uint32_t)32783u << 16;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[2] + (uint32_t)-12472;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)410));
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[17] = c->r[7] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
    c->r[18] = c->r[0] + (uint32_t)1;
    { int _t = (c->r[2] == c->r[18]); c->mem_w32((c->r[29] + (uint32_t)28), c->r[31]); if (_t) goto L_80052284; }
    c->r[2] = (uint32_t)32784u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-20121));
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80052284; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)2));
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80052284; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)6));
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80052284; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)4));
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[4] & 255u; if (_t) goto L_80052284; }
    c->mem_w8((c->r[16] + (uint32_t)4), (uint8_t)c->r[5]);
    c->r[5] = c->r[16] + (uint32_t)4;
    c->mem_w8((c->r[16] + (uint32_t)5), (uint8_t)c->r[6]);
    c->r[31] = 0x8005227Cu;
    c->r[6] = c->r[0] + (uint32_t)2; func_80087EAC(c);
    c->mem_w8((c->r[16] + (uint32_t)7), (uint8_t)c->r[17]);
    c->mem_w8((c->r[16] + (uint32_t)6), (uint8_t)c->r[18]);
  L_80052284:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

static void leaf_8005245C(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->r[4] = c->r[0] + c->r[0];
    c->r[2] = (uint32_t)32783u << 16;
    c->r[2] = c->r[2] + (uint32_t)-12472;
    c->r[5] = c->r[2] + (uint32_t)4;
    c->r[6] = c->r[0] + (uint32_t)2;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[31]);
    c->mem_w8((c->r[2] + (uint32_t)4), (uint8_t)c->r[0]);
    c->r[31] = 0x80052484u;
    c->mem_w8((c->r[2] + (uint32_t)5), (uint8_t)c->r[0]); func_80087EAC(c);
    c->r[31] = 0x8005248Cu;
    c->r[4] = c->r[0] + c->r[0]; func_80087AEC(c);
    c->r[3] = c->r[0] + (uint32_t)6;
    { int _t = (c->r[2] != c->r[3]); c->r[4] = c->r[0] + c->r[0]; if (_t) goto L_800524A4; }
    c->r[5] = (uint32_t)32778u << 16;
    c->r[31] = 0x800524A4u;
    c->r[5] = c->r[5] + (uint32_t)16288; func_80087E2C(c);
  L_800524A4:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_800525D0(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[17] = c->r[5] + c->r[0];
    c->r[5] = c->r[0] + (uint32_t)132;
    c->r[6] = c->r[0] + (uint32_t)3;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[31]);
    c->r[31] = 0x800525F8u;
    c->r[7] = c->r[0] + c->r[0]; func_80072DDC(c);
    c->r[3] = c->r[2] + c->r[0];
    { int _t = (c->r[3] == c->r[0]); c->r[2] = (uint32_t)32773u << 16; if (_t) goto L_80052614; }
    c->r[2] = c->r[2] + (uint32_t)10184;
    c->mem_w32((c->r[3] + (uint32_t)28), c->r[2]);
    c->mem_w8((c->r[3] + (uint32_t)3), (uint8_t)c->r[17]);
    c->mem_w32((c->r[3] + (uint32_t)16), c->r[16]);
  L_80052614:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[2] = c->r[3] + c->r[0];
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

static void leaf_8005262C(Core* c) {
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)68));
    c->r[2] = c->r[2] + (uint32_t)512;
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 6145);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)6144; if (_t) goto L_80052654; }
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[2]);
  L_80052654:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)74));
    c->r[2] = c->r[2] + (uint32_t)512;
    c->mem_w16((c->r[4] + (uint32_t)74), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 4097);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)4096; if (_t) goto L_8005267C; }
    c->mem_w16((c->r[4] + (uint32_t)74), (uint16_t)c->r[2]);
  L_8005267C:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)74));
    c->r[3] = c->mem_r32((c->r[4] + (uint32_t)48));
    c->r[2] = c->r[2] << 8;
    c->r[3] = c->r[3] - c->r[2];
    c->mem_w32((c->r[4] + (uint32_t)48), c->r[3]); return;
    return;
}

static void leaf_80052694(Core* c) {
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)68));
    c->r[2] = c->r[2] + (uint32_t)-64;
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 2560);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)2560; if (_t) goto L_800526BC; }
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[2]);
  L_800526BC:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)74));
    c->r[2] = c->r[2] + (uint32_t)160;
    c->mem_w16((c->r[4] + (uint32_t)74), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 4097);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)4096; if (_t) goto L_800526E4; }
    c->mem_w16((c->r[4] + (uint32_t)74), (uint16_t)c->r[2]);
  L_800526E4:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)74));
    c->r[3] = c->mem_r32((c->r[4] + (uint32_t)48));
    c->r[2] = c->r[2] << 8;
    c->r[3] = c->r[3] + c->r[2];
    c->mem_w32((c->r[4] + (uint32_t)48), c->r[3]);
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)50));
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)102));
    c->r[2] = (uint32_t)((int32_t)c->r[2] < (int32_t)c->r[3]);
    c->r[3] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)102));
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80052718; }
    c->mem_w16((c->r[4] + (uint32_t)50), (uint16_t)c->r[3]);
  L_80052718:;
     return;
    return;
}

static void leaf_80052720(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[5] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)100));
    c->r[6] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)104));
    c->r[31] = 0x80052744u;
    c->r[4] = c->r[16] + (uint32_t)44; func_800782B0(c);
    c->r[2] = c->r[2] << 16;
    c->r[4] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[5] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)86));
    c->r[31] = 0x80052758u;
    c->r[6] = c->r[0] + (uint32_t)24; func_800776F8(c);
    c->r[4] = c->r[2] << 16;
    c->r[4] = (uint32_t)((int32_t)c->r[4] >> 16);
    c->r[31] = 0x80052768u;
    c->mem_w16((c->r[16] + (uint32_t)86), (uint16_t)c->r[2]); func_80083F50(c);
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)68));
    { int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[3]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)86));
    c->r[7] = c->lo;
    c->r[31] = 0x80052784u;
    c->r[17] = (uint32_t)((int32_t)c->r[7] >> 4); func_80083E80(c);
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)68));
    { int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[3]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[2] = c->mem_r32((c->r[16] + (uint32_t)44));
    c->r[2] = c->r[2] + c->r[17];
    c->mem_w32((c->r[16] + (uint32_t)44), c->r[2]);
    c->r[2] = c->mem_r32((c->r[16] + (uint32_t)52));
    c->r[7] = c->lo;
    c->r[3] = (uint32_t)((int32_t)c->r[7] >> 4);
    c->r[2] = c->r[2] - c->r[3];
    c->mem_w32((c->r[16] + (uint32_t)52), c->r[2]);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

static void leaf_8005314C(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[31]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)106));
    c->r[2] = c->r[2] << 16;
    c->r[3] = (uint32_t)((int32_t)c->r[2] >> 28);
    c->r[4] = c->r[3] & 7u;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 24);
    c->r[3] = c->r[2] & 15u;
    c->r[2] = (uint32_t)(c->r[3] < (uint32_t)11);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)32769u << 16; if (_t) goto L_800531CC; }
    c->r[2] = c->r[2] + (uint32_t)23324;
    c->r[3] = c->r[3] << 2;
    c->r[3] = c->r[3] + c->r[2];
    c->r[2] = c->mem_r32((c->r[3] + (uint32_t)0));
    {  switch (c->r[2]) { case 0x800531A8u: goto L_800531A8; case 0x800531CCu: goto L_800531CC; case 0x80053198u: goto L_80053198; case 0x800531B8u: goto L_800531B8; default: rec_dispatch(c, c->r[2]); return; } }
  L_80053198:;
    { int _t = (c->r[4] != c->r[0]); c->r[4] = c->r[0] + (uint32_t)3; if (_t) goto L_800531AC; }
    c->r[4] = c->r[0] + (uint32_t)138; goto L_800531BC;
  L_800531A8:;
    c->r[4] = c->r[0] + (uint32_t)3;
  L_800531AC:;
    c->r[5] = c->r[0] + c->r[0];
    c->r[6] = c->r[5] + c->r[0]; goto L_800531C4;
  L_800531B8:;
    c->r[4] = c->r[0] + (uint32_t)144;
  L_800531BC:;
    c->r[5] = c->r[0] + (uint32_t)11;
    c->r[6] = c->r[0] + (uint32_t)-50;
  L_800531C4:;
    c->r[31] = 0x800531CCu;
     func_80074590(c);
  L_800531CC:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_800532A0(Core* c) {
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)106));
    c->r[3] = c->r[2] & 3840u;
    c->r[5] = c->r[2] >> 12;
    c->r[3] = c->r[3] >> 8;
    c->r[2] = (uint32_t)((int32_t)c->r[3] < 4);
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_800532D8; }
    { int _t = (c->r[3] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)3; if (_t) goto L_80053378; }
    { int _t = (c->r[3] == c->r[0]); c->r[3] = (uint32_t)32778u << 16; if (_t) goto L_800532EC; }
     goto L_800533EC;
  L_800532D8:;
    c->r[2] = c->r[0] + (uint32_t)5;
    { int _t = (c->r[3] == c->r[2]); c->r[3] = (uint32_t)32778u << 16; if (_t) goto L_8005339C; }
     goto L_800533EC;
  L_800532EC:;
    c->r[6] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)322));
    c->r[3] = c->r[5] + c->r[0];
    c->r[2] = c->r[0] + (uint32_t)2;
    { int _t = (c->r[3] != c->r[2]); c->r[7] = c->r[6] >> 31; if (_t) goto L_8005332C; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)327));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[7] + (uint32_t)4; if (_t) goto L_800533E4; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)68));
    c->r[2] = c->r[2] << 16;
    { int _t = ((int32_t)c->r[2] <= 0); c->r[2] = (uint32_t)((int32_t)c->r[2] >> 17); if (_t) goto L_800533E8; }
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[2]); goto L_800533E8;
  L_8005332C:;
    c->r[2] = c->r[0] + (uint32_t)3;
    { int _t = (c->r[3] != c->r[2]);  if (_t) goto L_800533E8; }
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)68));
    { int _t = ((int32_t)c->r[2] >= 0);  if (_t) goto L_8005334C; }
    c->r[2] = c->r[0] - c->r[2];
  L_8005334C:;
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 5376);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[7] + (uint32_t)14; if (_t) goto L_800533E4; }
    { int _t = ((int32_t)c->r[6] >= 0); c->r[2] = c->r[6] + c->r[0]; if (_t) goto L_80053364; }
    c->r[2] = c->r[0] - c->r[2];
  L_80053364:;
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 240);
    { int _t = (c->r[2] != c->r[0]); c->r[3] = (uint32_t)32778u << 16; if (_t) goto L_800533EC; }
    c->r[2] = c->r[7] + (uint32_t)14; goto L_800533E4;
  L_80053378:;
    { int _t = (c->r[5] != c->r[2]); c->r[3] = (uint32_t)32778u << 16; if (_t) goto L_800533EC; }
    c->r[2] = (uint32_t)32780u << 16;
    c->r[3] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-1936));
    c->r[2] = c->r[0] + (uint32_t)4;
    { int _t = (c->r[3] != c->r[2]); c->r[3] = (uint32_t)32778u << 16; if (_t) goto L_800533EC; }
    c->r[2] = c->r[0] + (uint32_t)6; goto L_800533E4;
  L_8005339C:;
    c->r[3] = c->r[5] + c->r[0];
    c->r[2] = c->r[0] + (uint32_t)3;
    { int _t = (c->r[3] != c->r[2]); c->r[2] = c->r[0] + (uint32_t)4; if (_t) goto L_800533C8; }
    c->r[2] = (uint32_t)32780u << 16;
    c->r[3] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-1936));
    c->r[2] = c->r[0] + (uint32_t)4;
    { int _t = (c->r[3] != c->r[2]); c->r[3] = (uint32_t)32778u << 16; if (_t) goto L_800533EC; }
    c->r[2] = c->r[0] + (uint32_t)6; goto L_800533E4;
  L_800533C8:;
    { int _t = (c->r[3] != c->r[2]); c->r[3] = (uint32_t)32778u << 16; if (_t) goto L_800533EC; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)382));
    c->r[2] = c->r[2] & 48u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)8; if (_t) goto L_800533EC; }
  L_800533E4:;
    c->mem_w8((c->r[4] + (uint32_t)362), (uint8_t)c->r[2]);
  L_800533E8:;
    c->r[3] = (uint32_t)32778u << 16;
  L_800533EC:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)362));
    c->r[3] = c->r[3] + (uint32_t)17528;
    c->r[2] = c->r[2] & 14u;
    c->r[2] = c->r[2] >> 1;
    c->r[2] = c->r[2] + c->r[3];
    c->r[3] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)0));
    c->r[2] = c->r[3] & 255u;
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80053440; }
    c->r[5] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)5));
    { int _t = (c->r[5] == c->r[2]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_80053434; }
    c->mem_w8((c->r[4] + (uint32_t)5), (uint8_t)c->r[3]);
    c->mem_w8((c->r[4] + (uint32_t)6), (uint8_t)c->r[0]);
    c->mem_w8((c->r[4] + (uint32_t)7), (uint8_t)c->r[0]); return;
  L_80053434:;
    c->r[3] = c->r[0] + (uint32_t)51;
    { int _t = (c->r[5] == c->r[3]);  if (_t) goto L_80053444; }
  L_80053440:;
    c->r[2] = c->r[0] + c->r[0];
  L_80053444:;
     return;
    return;
}

static void leaf_800538E0(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-40;
    { int _t = (c->r[6] != c->r[0]); c->mem_w32((c->r[29] + (uint32_t)32), c->r[31]); if (_t) goto L_80053958; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)106));
    c->r[2] = c->r[2] >> 8;
    c->r[3] = c->r[2] & 15u;
    { int _t = (c->r[3] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)3; if (_t) goto L_8005390C; }
    { int _t = (c->r[3] != c->r[2]);  if (_t) goto L_80053958; }
  L_8005390C:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)2));
    c->r[6] = c->r[0] + (uint32_t)-90;
    c->mem_w16((c->r[29] + (uint32_t)18), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)6));
    c->r[3] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)132));
    c->r[4] = c->r[0] + (uint32_t)53;
    c->r[2] = c->r[2] + c->r[3];
    c->mem_w16((c->r[29] + (uint32_t)22), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)10));
    c->r[5] = c->r[29] + (uint32_t)16;
    c->r[31] = 0x8005393Cu;
    c->mem_w16((c->r[29] + (uint32_t)26), (uint16_t)c->r[2]); func_8003116C(c);
    c->r[3] = c->r[2] + c->r[0];
    { int _t = (c->r[3] == c->r[0]);  if (_t) goto L_80053958; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)40));
    c->r[2] = c->r[2] | 128u;
    c->mem_w8((c->r[3] + (uint32_t)40), (uint8_t)c->r[2]);
  L_80053958:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[29] = c->r[29] + (uint32_t)40; return;
    return;
}

static void leaf_80053D0C(Core* c) {
    c->r[5] = c->mem_r32((c->r[4] + (uint32_t)344));
    { int _t = (c->r[5] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_80053D84; }
    { int _t = (c->r[5] == c->r[2]); c->r[2] = c->r[0] + (uint32_t)4; if (_t) goto L_80053D84; }
    c->r[3] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)12));
    { int _t = (c->r[3] == c->r[2]); c->r[2] = (uint32_t)((int32_t)c->r[3] < 5); if (_t) goto L_80053D68; }
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)2; if (_t) goto L_80053D4C; }
    { int _t = (c->r[3] == c->r[2]);  if (_t) goto L_80053D60; }
    c->mem_w32((c->r[4] + (uint32_t)344), c->r[0]); goto L_80053D88;
  L_80053D4C:;
    c->r[2] = c->r[0] + (uint32_t)5;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = c->r[0] + (uint32_t)9; if (_t) goto L_80053D60; }
    { int _t = (c->r[3] != c->r[2]);  if (_t) goto L_80053D84; }
  L_80053D60:;
    c->mem_w8((c->r[5] + (uint32_t)94), (uint8_t)c->r[0]); goto L_80053D84;
  L_80053D68:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)2));
    c->r[2] = c->r[2] + (uint32_t)-47;
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)2);
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80053D84; }
    c->mem_w8((c->r[5] + (uint32_t)41), (uint8_t)c->r[0]);
  L_80053D84:;
    c->mem_w32((c->r[4] + (uint32_t)344), c->r[0]);
  L_80053D88:;
    c->mem_w8((c->r[4] + (uint32_t)356), (uint8_t)c->r[0]); return;
    return;
}

static void leaf_80053D90(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[31]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)120));
    c->r[3] = c->r[2] + (uint32_t)-1;
    c->r[2] = (uint32_t)(c->r[3] < (uint32_t)9);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)32769u << 16; if (_t) goto L_80053E30; }
    c->r[2] = c->r[2] + (uint32_t)23540;
    c->r[3] = c->r[3] << 2;
    c->r[3] = c->r[3] + c->r[2];
    c->r[2] = c->mem_r32((c->r[3] + (uint32_t)0));
    {  switch (c->r[2]) { case 0x80053DCCu: goto L_80053DCC; case 0x80053E00u: goto L_80053E00; case 0x80053E14u: goto L_80053E14; default: rec_dispatch(c, c->r[2]); return; } }
  L_80053DCC:;
    c->r[5] = c->mem_r32((c->r[4] + (uint32_t)16));
    c->r[2] = c->r[0] + (uint32_t)2;
    c->mem_w8((c->r[5] + (uint32_t)0), (uint8_t)c->r[2]);
    c->r[3] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)86));
    c->mem_w8((c->r[5] + (uint32_t)4), (uint8_t)c->r[2]);
    c->mem_w8((c->r[5] + (uint32_t)5), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)40));
    c->mem_w8((c->r[5] + (uint32_t)6), (uint8_t)c->r[0]);
    c->r[3] = c->r[3] >> 4;
    c->r[2] = c->r[2] | 128u;
    c->mem_w8((c->r[5] + (uint32_t)43), (uint8_t)c->r[3]);
    c->mem_w8((c->r[5] + (uint32_t)40), (uint8_t)c->r[2]); goto L_80053E30;
  L_80053E00:;
    c->r[5] = c->mem_r32((c->r[4] + (uint32_t)16));
    c->r[2] = c->r[0] + (uint32_t)2;
    c->mem_w8((c->r[5] + (uint32_t)4), (uint8_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)3; goto L_80053E28;
  L_80053E14:;
    c->r[5] = c->mem_r32((c->r[4] + (uint32_t)16));
    c->r[2] = c->r[0] + (uint32_t)2;
    c->mem_w8((c->r[5] + (uint32_t)0), (uint8_t)c->r[2]);
    c->mem_w8((c->r[5] + (uint32_t)4), (uint8_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)1;
  L_80053E28:;
    c->mem_w8((c->r[5] + (uint32_t)5), (uint8_t)c->r[2]);
    c->mem_w8((c->r[5] + (uint32_t)6), (uint8_t)c->r[0]);
  L_80053E30:;
    c->mem_w8((c->r[4] + (uint32_t)324), (uint8_t)c->r[0]);
    c->mem_w8((c->r[4] + (uint32_t)120), (uint8_t)c->r[0]);
    c->r[31] = 0x80053E40u;
    c->mem_w8((c->r[4] + (uint32_t)385), (uint8_t)c->r[0]); func_80053D0C(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_800541F4(Core* c) {
    c->r[2] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)595));
    c->r[29] = c->r[29] + (uint32_t)-40;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
    c->r[18] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[19]);
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[3] = c->r[3] & 4u;
    c->mem_w8((c->r[2] + (uint32_t)595), (uint8_t)c->r[3]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)363));
    { int _t = (c->r[2] != c->r[0]); c->r[19] = c->r[5] + c->r[0]; if (_t) goto L_80054300; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)325));
    { int _t = (c->r[2] == c->r[0]); c->r[3] = c->r[0] + (uint32_t)-130; if (_t) goto L_80054258; }
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[18] + (uint32_t)74));
    c->r[2] = (uint32_t)((int32_t)c->r[2] < -9984);
    { int _t = (c->r[2] == c->r[0]); c->r[3] = c->r[0] + (uint32_t)-90; if (_t) goto L_80054258; }
    c->r[3] = c->r[0] + (uint32_t)-120;
  L_80054258:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[18] + (uint32_t)382));
    { int _t = ((int32_t)c->r[2] >= 0); c->r[16] = c->r[0] + (uint32_t)56; if (_t) goto L_80054280; }
    c->r[2] = c->r[3] << 16;
    c->r[3] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = c->r[2] >> 31;
    c->r[3] = c->r[3] + c->r[2];
    c->r[3] = c->r[3] >> 1;
    c->r[16] = c->r[0] + (uint32_t)28;
  L_80054280:;
    c->r[2] = c->r[19] & 1u;
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[18] + c->r[0]; if (_t) goto L_800542BC; }
    c->r[5] = c->r[16] + c->r[0];
    c->r[2] = c->r[3] << 16;
    c->r[17] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[31] = 0x800542A0u;
    c->r[6] = c->r[17] + c->r[0]; func_800493E8(c);
    c->r[3] = c->r[2] + c->r[0];
    { int _t = (c->r[3] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)2; if (_t) goto L_800542DC; }
    c->r[4] = c->r[18] + c->r[0];
    c->r[5] = c->r[0] - c->r[16];
    c->r[6] = c->r[17] + c->r[0]; goto L_800542C8;
  L_800542BC:;
    c->r[5] = c->r[0] + c->r[0];
    c->r[6] = c->r[3] << 16;
    c->r[6] = (uint32_t)((int32_t)c->r[6] >> 16);
  L_800542C8:;
    c->r[31] = 0x800542D0u;
     func_800493E8(c);
    c->r[3] = c->r[2] + c->r[0];
    { int _t = (c->r[3] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)2; if (_t) goto L_80054300; }
  L_800542DC:;
    { int _t = (c->r[3] != c->r[2]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_80054308; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)422));
    c->r[3] = c->r[0] + (uint32_t)256;
    c->r[2] = c->r[2] & 3840u;
    { int _t = (c->r[2] != c->r[3]); c->r[3] = (uint32_t)8064u << 16; if (_t) goto L_80054300; }
    c->r[2] = c->r[0] + (uint32_t)1;
    c->mem_w8((c->r[3] + (uint32_t)595), (uint8_t)c->r[2]);
  L_80054300:;
    c->r[2] = c->r[0] + c->r[0]; goto L_800543A4;
  L_80054308:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)325));
    c->r[2] = c->r[2] & 1u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[19] & 2u; if (_t) goto L_80054330; }
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[18] + (uint32_t)74));
    { int _t = ((int32_t)c->r[2] >= 0); c->r[2] = c->r[19] & 2u; if (_t) goto L_80054330; }
    c->mem_w16((c->r[18] + (uint32_t)74), (uint16_t)c->r[0]);
  L_80054330:;
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_800543A4; }
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)422));
    c->r[2] = c->r[2] & 32768u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)32780u << 16; if (_t) goto L_80054394; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-1933));
    { int _t = (c->r[2] != c->r[0]); c->r[3] = (uint32_t)8064u << 16; if (_t) goto L_80054398; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[18] + (uint32_t)382));
    c->r[2] = c->r[2] & 512u;
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)2; if (_t) goto L_8005439C; }
    c->r[4] = c->r[18] + c->r[0];
    c->r[5] = c->r[0] + c->r[0];
    c->r[6] = c->r[0] + (uint32_t)1;
    c->r[31] = 0x80054388u;
    c->r[7] = c->r[6] + c->r[0]; func_80022D08(c);
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = c->r[0] + (uint32_t)2; goto L_8005439C;
  L_80054394:;
    c->r[3] = (uint32_t)8064u << 16;
  L_80054398:;
    c->r[2] = c->r[0] + (uint32_t)3;
  L_8005439C:;
    c->mem_w8((c->r[3] + (uint32_t)595), (uint8_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)1;
  L_800543A4:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)40; return;
    return;
}

static void leaf_800543C0(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->r[7] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[31]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)363));
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_8005443C; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)41));
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_8005443C; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)120));
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80054420; }
    c->r[2] = c->mem_r32((c->r[7] + (uint32_t)16));
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[7] + (uint32_t)50));
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)134));
    c->r[5] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)132));
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)50));
    c->r[4] = c->r[4] - c->r[5];
    c->r[3] = c->r[3] - c->r[2];
    c->r[4] = c->r[4] - c->r[3]; goto L_80054424;
  L_80054420:;
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[7] + (uint32_t)98));
  L_80054424:;
    c->r[6] = c->r[4] << 16;
    c->r[4] = c->r[7] + c->r[0];
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x8005443Cu;
    c->r[6] = (uint32_t)((int32_t)c->r[6] >> 16); func_80049250(c);
  L_8005443C:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_8005444C(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-40;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[19]);
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)363));
    c->r[19] = c->r[0] + c->r[0];
    c->mem_w16((c->r[16] + (uint32_t)364), (uint16_t)c->r[0]);
    { int _t = (c->r[2] != c->r[0]); c->mem_w16((c->r[16] + (uint32_t)106), (uint16_t)c->r[0]); if (_t) goto L_80054630; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)41));
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80054634; }
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)382));
    { int _t = ((int32_t)c->r[2] < 0);  if (_t) goto L_800544A8; }
    c->r[18] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)128));
     goto L_800544C4;
  L_800544A8:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)128));
    c->r[2] = c->r[2] << 16;
    c->r[3] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = c->r[2] >> 31;
    c->r[3] = c->r[3] + c->r[2];
    c->r[18] = c->r[3] >> 1;
  L_800544C4:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)120));
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_800544F8; }
    c->r[2] = c->mem_r32((c->r[16] + (uint32_t)16));
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)50));
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)134));
    c->r[5] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)132));
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)50));
    c->r[4] = c->r[4] - c->r[5];
    c->r[3] = c->r[3] - c->r[2];
    c->r[6] = c->r[4] - c->r[3]; goto L_800544FC;
  L_800544F8:;
    c->r[6] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)98));
  L_800544FC:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)97));
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_80054538; }
    c->r[5] = c->r[0] + c->r[0];
    c->r[6] = c->r[6] << 16;
    c->r[6] = (uint32_t)((int32_t)c->r[6] >> 16);
  L_80054518:;
    c->r[31] = 0x80054520u;
     func_80049250(c);
    c->mem_w8((c->r[16] + (uint32_t)41), (uint8_t)c->r[2]);
    c->r[2] = c->r[2] & 255u;
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_8005459C; }
     goto L_80054630;
  L_80054538:;
    c->r[5] = c->r[0] + c->r[0];
    c->r[2] = c->r[6] << 16;
    c->r[17] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[31] = 0x8005454Cu;
    c->r[6] = c->r[17] + c->r[0]; func_80049250(c);
    c->mem_w8((c->r[16] + (uint32_t)41), (uint8_t)c->r[2]);
    c->r[2] = c->r[2] & 255u;
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_8005459C; }
    c->r[19] = c->r[0] + (uint32_t)1;
    c->r[4] = c->r[16] + c->r[0];
    c->r[5] = c->r[18] << 16;
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16);
    c->r[31] = 0x80054574u;
    c->r[6] = c->r[17] + c->r[0]; func_80049250(c);
    c->mem_w8((c->r[16] + (uint32_t)41), (uint8_t)c->r[2]);
    c->r[2] = c->r[2] & 255u;
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_8005459C; }
    c->r[4] = c->r[16] + c->r[0];
    c->r[5] = c->r[0] - c->r[18];
    c->r[5] = c->r[5] << 16;
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16);
    c->r[6] = c->r[17] + c->r[0]; goto L_80054518;
  L_8005459C:;
    c->r[3] = (uint32_t)8064u << 16;
    c->r[4] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)420));
    c->r[3] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)422));
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[2] + (uint32_t)414), (uint16_t)c->r[4]);
    c->r[2] = c->r[0] + (uint32_t)1;
    { int _t = (c->r[19] != c->r[2]); c->mem_w16((c->r[16] + (uint32_t)106), (uint16_t)c->r[3]); if (_t) goto L_800545E0; }
    c->r[2] = c->r[3] & 32768u;
    { int _t = (c->r[2] == c->r[0]); c->r[3] = c->r[3] & 3840u; if (_t) goto L_800545E0; }
    c->r[2] = c->r[0] + (uint32_t)256;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = c->r[0] + (uint32_t)512; if (_t) goto L_800545DC; }
    { int _t = (c->r[3] != c->r[2]);  if (_t) goto L_800545E0; }
  L_800545DC:;
    c->mem_w16((c->r[16] + (uint32_t)106), (uint16_t)c->r[0]);
  L_800545E0:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)41));
    c->r[2] = c->r[2] & 1u;
    { int _t = (c->r[2] == c->r[0]); c->mem_w8((c->r[16] + (uint32_t)41), (uint8_t)c->r[2]); if (_t) goto L_80054630; }
    c->r[2] = c->r[2] | 128u;
    c->mem_w16((c->r[16] + (uint32_t)80), (uint16_t)c->r[0]);
    { int _t = (c->r[19] != c->r[0]); c->mem_w8((c->r[16] + (uint32_t)41), (uint8_t)c->r[2]); if (_t) goto L_80054614; }
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)424));
    c->mem_w16((c->r[16] + (uint32_t)364), (uint16_t)c->r[2]); goto L_80054618;
  L_80054614:;
    c->mem_w16((c->r[16] + (uint32_t)364), (uint16_t)c->r[0]);
  L_80054618:;
    c->r[31] = 0x80054620u;
    c->r[4] = c->r[16] + c->r[0]; func_80049674(c);
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)418));
    c->mem_w16((c->r[16] + (uint32_t)322), (uint16_t)c->r[2]);
  L_80054630:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)41));
  L_80054634:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)40; return;
    return;
}

static void leaf_80054E80(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[17] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)325));
    { int _t = (c->r[2] != c->r[0]); c->r[7] = c->r[5] + c->r[0]; if (_t) goto L_80054F48; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)96));
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80054F48; }
    c->r[4] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)95));
    { int _t = (c->r[4] == c->r[0]);  if (_t) goto L_80054F48; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)330));
    c->r[2] = c->r[2] & 2u;
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[4] & 1u; if (_t) goto L_80054F48; }
    c->r[3] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)327));
    { int _t = (c->r[2] != c->r[3]);  if (_t) goto L_80054F48; }
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[17] + (uint32_t)68));
    c->r[6] = (uint32_t)c->mem_r16((c->r[17] + (uint32_t)68));
    { int _t = ((int32_t)c->r[2] >= 0); c->r[2] = c->r[6] << 16; if (_t) goto L_80054F00; }
    c->r[6] = c->r[0] - c->r[6];
    c->r[2] = c->r[6] << 16;
  L_80054F00:;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 3329);
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[17] + c->r[0]; if (_t) goto L_80054F48; }
    c->r[5] = c->r[0] + (uint32_t)198;
    c->r[31] = 0x80054F1Cu;
    c->r[6] = c->r[0] + (uint32_t)4; func_80054D14(c);
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)380));
    c->r[2] = c->r[2] & 15u;
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[0] + (uint32_t)59; if (_t) goto L_800551B0; }
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x80054F40u;
    c->r[6] = c->r[5] + c->r[0]; func_80074590(c);
     goto L_800551B0;
  L_80054F48:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)329));
    c->r[2] = c->r[2] & 4u;
    { int _t = (c->r[2] == c->r[0]); c->r[16] = c->r[0] + (uint32_t)110; if (_t) goto L_80054FA8; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)70));
    { int _t = (c->r[2] == c->r[16]); c->r[4] = c->r[0] + (uint32_t)28; if (_t) goto L_800551B0; }
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x80054F78u;
    c->r[6] = c->r[5] + c->r[0]; func_80074590(c);
    c->r[4] = c->r[17] + c->r[0];
    c->r[5] = c->r[16] + c->r[0];
    c->r[31] = 0x80054F88u;
    c->mem_w8((c->r[17] + (uint32_t)70), (uint8_t)c->r[16]); func_80054790(c);
    c->r[4] = c->r[17] + c->r[0];
    c->r[5] = (uint32_t)32769u << 16;
    c->r[5] = c->r[5] + (uint32_t)32744;
    c->r[7] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)150));
    c->r[31] = 0x80054FA0u;
    c->r[6] = c->r[16] + c->r[0]; func_80077CFC(c);
     goto L_800551B0;
  L_80054FA8:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)330));
    c->r[2] = c->r[2] & 2u;
    { int _t = (c->r[2] == c->r[0]); c->r[3] = c->r[0] + (uint32_t)1280; if (_t) goto L_80054FE8; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[17] + (uint32_t)106));
    c->r[2] = c->r[2] & 3840u;
    { int _t = (c->r[2] != c->r[3]);  if (_t) goto L_80054FE8; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)357));
    { int _t = (c->r[2] == c->r[0]); c->r[16] = c->r[0] + (uint32_t)2; if (_t) goto L_8005504C; }
    c->r[16] = c->r[0] + (uint32_t)17; goto L_8005504C;
  L_80054FE8:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[17] + (uint32_t)68));
    c->r[6] = (uint32_t)c->mem_r16((c->r[17] + (uint32_t)68));
    { int _t = ((int32_t)c->r[2] >= 0);  if (_t) goto L_80054FFC; }
    c->r[6] = c->r[0] - c->r[6];
  L_80054FFC:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)357));
    { int _t = (c->r[2] != c->r[0]); c->r[16] = c->r[0] + (uint32_t)16; if (_t) goto L_8005504C; }
    c->r[2] = c->r[6] << 16;
    c->r[3] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[3] < 3088);
    { int _t = (c->r[2] != c->r[0]); c->r[16] = c->r[0] + c->r[0]; if (_t) goto L_80055030; }
    c->r[2] = (uint32_t)((int32_t)c->r[3] < 4368);
    { int _t = (c->r[2] == c->r[0]); c->r[16] = c->r[0] + (uint32_t)4; if (_t) goto L_80055030; }
    c->r[16] = c->r[0] + (uint32_t)2;
  L_80055030:;
    c->r[3] = (uint32_t)32778u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)109));
    c->r[3] = c->r[3] + (uint32_t)17740;
    c->r[2] = c->r[2] >> 1;
    c->r[2] = c->r[2] + c->r[16];
    c->r[2] = c->r[2] + c->r[3];
    c->r[16] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)0));
  L_8005504C:;
    c->r[3] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)70));
    c->r[2] = c->r[16] & 255u;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = c->r[0] + (uint32_t)110; if (_t) goto L_800551B0; }
    { int _t = (c->r[3] != c->r[2]);  if (_t) goto L_800550B0; }
    c->r[3] = c->r[16] + (uint32_t)-4;
    c->r[2] = (uint32_t)(c->r[3] < (uint32_t)13);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)32769u << 16; if (_t) goto L_80055094; }
    c->r[2] = c->r[2] + (uint32_t)23580;
    c->r[3] = c->r[3] << 2;
    c->r[3] = c->r[3] + c->r[2];
    c->r[2] = c->mem_r32((c->r[3] + (uint32_t)0));
    {  switch (c->r[2]) { case 0x80055090u: goto L_80055090; case 0x80055094u: goto L_80055094; default: rec_dispatch(c, c->r[2]); return; } }
  L_80055090:;
    c->r[16] = c->r[16] + (uint32_t)-1;
  L_80055094:;
    c->r[4] = c->r[17] + c->r[0];
    c->r[5] = (uint32_t)32769u << 16;
    c->r[5] = c->r[5] + (uint32_t)32744;
    c->r[31] = 0x800550A8u;
    c->r[6] = c->r[16] + c->r[0]; func_80077C40(c);
    c->mem_w8((c->r[17] + (uint32_t)70), (uint8_t)c->r[16]); goto L_800551A4;
  L_800550B0:;
    { int _t = (c->r[7] != c->r[0]); c->r[4] = c->r[17] + c->r[0]; if (_t) goto L_80055184; }
    c->r[2] = c->r[0] + (uint32_t)16;
    { int _t = (c->r[3] != c->r[2]);  if (_t) goto L_80055104; }
    { int _t = (c->r[16] == c->r[2]); c->r[5] = (uint32_t)32769u << 16; if (_t) goto L_800550F4; }
    c->r[2] = c->mem_r32((c->r[17] + (uint32_t)56));
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)6));
    c->r[2] = c->r[2] & 16384u;
    { int _t = (c->r[2] == c->r[0]); c->r[6] = c->r[16] + c->r[0]; if (_t) goto L_800551B0; }
    c->r[5] = c->r[5] + (uint32_t)32744;
    c->r[7] = c->r[0] + (uint32_t)4; goto L_80055198;
  L_800550F4:;
    c->r[5] = c->r[5] + (uint32_t)32744;
    c->r[6] = c->r[0] + (uint32_t)16;
    c->r[7] = c->r[0] + (uint32_t)2; goto L_80055198;
  L_80055104:;
    { int _t = (c->r[16] != c->r[2]); c->r[3] = c->r[0] + (uint32_t)1; if (_t) goto L_80055118; }
    c->r[7] = c->r[0] + (uint32_t)1;
    c->r[6] = c->r[0] + (uint32_t)2; goto L_8005514C;
  L_80055118:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[17] + (uint32_t)14));
    c->r[2] = c->r[2] & 127u;
    { int _t = (c->r[2] != c->r[3]);  if (_t) goto L_800551B0; }
    c->r[31] = 0x80055134u;
    c->r[4] = c->r[17] + c->r[0]; func_80076D68(c);
    c->r[2] = c->mem_r32((c->r[17] + (uint32_t)56));
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)2));
    c->r[6] = c->r[0] + (uint32_t)5;
    c->r[2] = c->r[2] + (uint32_t)1;
    c->r[7] = c->r[2] & 3u;
  L_8005514C:;
    c->r[4] = c->r[17] + c->r[0];
    c->r[6] = c->r[6] << 16;
    c->r[6] = (uint32_t)((int32_t)c->r[6] >> 16);
    c->r[3] = (uint32_t)32769u << 16;
    c->r[3] = c->r[3] + (uint32_t)32744;
    c->r[2] = c->r[16] << 2;
    c->r[2] = c->r[2] + c->r[3];
    c->r[5] = c->mem_r32((c->r[2] + (uint32_t)0));
    c->r[2] = c->r[7] << 3;
    c->r[5] = c->r[5] + c->r[2];
    c->r[31] = 0x8005517Cu;
    c->mem_w32((c->r[17] + (uint32_t)56), c->r[5]); func_80077E3C(c);
    c->mem_w8((c->r[17] + (uint32_t)70), (uint8_t)c->r[16]); goto L_800551A4;
  L_80055184:;
    c->r[5] = (uint32_t)32769u << 16;
    c->r[5] = c->r[5] + (uint32_t)32744;
    c->r[6] = c->r[16] + c->r[0];
    c->r[7] = c->r[7] << 16;
    c->r[7] = (uint32_t)((int32_t)c->r[7] >> 16);
  L_80055198:;
    c->r[31] = 0x800551A0u;
     func_80077CFC(c);
    c->mem_w8((c->r[17] + (uint32_t)70), (uint8_t)c->r[16]);
  L_800551A4:;
    c->r[4] = c->r[17] + c->r[0];
    c->r[31] = 0x800551B0u;
    c->r[5] = c->r[16] + c->r[0]; func_80054790(c);
  L_800551B0:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

static void leaf_800551C4(Core* c) {
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)41));
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80055248; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)327));
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_8005521C; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)320));
    c->r[3] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)325));
    c->r[2] = c->r[2] + (uint32_t)-2048;
    c->r[2] = c->r[2] & 4095u;
    { int _t = (c->r[3] != c->r[0]); c->mem_w16((c->r[4] + (uint32_t)86), (uint16_t)c->r[2]); if (_t) goto L_8005527C; }
    c->r[3] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)322));
    c->r[2] = c->r[0] + (uint32_t)4096;
    c->r[3] = c->r[3] << 16;
    c->r[3] = (uint32_t)((int32_t)c->r[3] >> 17);
    c->r[2] = c->r[2] - c->r[3];
    c->r[2] = c->r[2] & 4095u;
    c->mem_w16((c->r[4] + (uint32_t)88), (uint16_t)c->r[2]); return;
  L_8005521C:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)320));
    c->r[3] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)325));
    { int _t = (c->r[3] != c->r[0]); c->mem_w16((c->r[4] + (uint32_t)86), (uint16_t)c->r[2]); if (_t) goto L_8005527C; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)322));
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 17);
    c->mem_w16((c->r[4] + (uint32_t)88), (uint16_t)c->r[2]); return;
  L_80055248:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)327));
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80055270; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)320));
    c->r[2] = c->r[2] + (uint32_t)-2048;
    c->r[2] = c->r[2] & 4095u;
    c->mem_w16((c->r[4] + (uint32_t)86), (uint16_t)c->r[2]); return;
  L_80055270:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)320));
    c->mem_w16((c->r[4] + (uint32_t)86), (uint16_t)c->r[2]);
  L_8005527C:;
     return;
    return;
}

static void leaf_80055284(Core* c) {
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)327));
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_800552AC; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)320));
    c->r[2] = c->r[2] + (uint32_t)-2048;
    c->r[2] = c->r[2] & 4095u;
    c->mem_w16((c->r[4] + (uint32_t)86), (uint16_t)c->r[2]); return;
  L_800552AC:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)320));
    c->mem_w16((c->r[4] + (uint32_t)86), (uint16_t)c->r[2]); return;
    return;
}

static void leaf_80055634(Core* c) {
    c->r[2] = (uint32_t)32780u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-2026));
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    { int _t = (c->r[2] != c->r[0]); c->mem_w32((c->r[29] + (uint32_t)20), c->r[31]); if (_t) goto L_800556F0; }
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)560));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + c->r[0]; if (_t) goto L_800556F4; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)372));
    c->r[2] = c->r[2] & 7u;
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + c->r[0]; if (_t) goto L_800556F4; }
    c->r[2] = (uint32_t)32782u << 16;
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)32360));
    c->r[3] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)372));
    c->r[2] = c->r[2] & c->r[3];
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_800556B0; }
    c->mem_w8((c->r[16] + (uint32_t)373), (uint8_t)c->r[0]);
    c->r[31] = 0x800556A4u;
    c->r[5] = c->r[0] + c->r[0]; func_800535E0(c);
    c->r[2] = (uint32_t)(c->r[0] < c->r[2]);
    c->r[2] = c->r[2] << 1; goto L_800556F4;
  L_800556B0:;
    { int _t = (c->r[5] != c->r[0]); c->r[2] = c->r[0] + c->r[0]; if (_t) goto L_800556F4; }
    c->r[2] = (uint32_t)32783u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)-12460));
    c->r[2] = c->r[2] & c->r[3];
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_800556EC; }
    c->r[31] = 0x800556D8u;
    c->r[5] = c->r[0] + (uint32_t)1; func_800535E0(c);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_800556EC; }
    c->mem_w8((c->r[16] + (uint32_t)373), (uint8_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)2; goto L_800556F4;
  L_800556EC:;
    c->mem_w8((c->r[16] + (uint32_t)373), (uint8_t)c->r[0]);
  L_800556F0:;
    c->r[2] = c->r[0] + c->r[0];
  L_800556F4:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_80055824(Core* c) {
    c->r[2] = (uint32_t)32782u << 16;
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)32360));
    c->r[3] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)370));
    c->r[2] = c->r[2] & c->r[3];
    c->r[2] = (uint32_t)(c->r[0] < c->r[2]); return;
    return;
}

static void leaf_80055D5C(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[31]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)363));
    { int _t = (c->r[3] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)8; if (_t) goto L_80055DD8; }
    { int _t = (c->r[3] != c->r[2]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_80055D94; }
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)528));
    c->mem_w16((c->r[16] + (uint32_t)320), (uint16_t)c->r[2]); goto L_80055DA0;
  L_80055D94:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)396));
    c->mem_w16((c->r[16] + (uint32_t)320), (uint16_t)c->r[2]);
  L_80055DA0:;
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)320));
    c->r[31] = 0x80055DACu;
     func_80083F50(c);
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)320));
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 4);
    c->r[31] = 0x80055DBCu;
    c->mem_w16((c->r[16] + (uint32_t)72), (uint16_t)c->r[2]); func_80083E80(c);
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 4);
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)41));
    c->r[2] = c->r[0] - c->r[2];
    { int _t = (c->r[3] == c->r[0]); c->mem_w16((c->r[16] + (uint32_t)76), (uint16_t)c->r[2]); if (_t) goto L_80055E18; }
    c->mem_w16((c->r[16] + (uint32_t)74), (uint16_t)c->r[0]); goto L_80055E18;
  L_80055DD8:;
    c->r[31] = 0x80055DE0u;
    c->r[4] = c->r[16] + c->r[0]; func_8004766C(c);
    c->r[31] = 0x80055DE8u;
    c->r[4] = c->r[16] + c->r[0]; func_80049760(c);
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)416));
    c->mem_w16((c->r[16] + (uint32_t)320), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)311));
    c->r[2] = c->r[0] + (uint32_t)1;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_80055E18; }
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)42));
    c->mem_w8((c->r[2] + (uint32_t)519), (uint8_t)c->r[3]);
  L_80055E18:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_80055E28(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[31]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)330));
    c->r[4] = (uint32_t)32783u << 16;
    c->r[3] = c->r[2] & 1u;
    c->mem_w8((c->r[16] + (uint32_t)330), (uint8_t)c->r[3]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)-12460));
    c->r[2] = c->r[2] & 160u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_80055EC0; }
    c->r[3] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)-12460));
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)364));
    c->r[2] = c->r[3] & c->r[2];
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_80055E7C; }
    c->mem_w8((c->r[16] + (uint32_t)330), (uint8_t)c->r[0]); goto L_80055E94;
  L_80055E7C:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)366));
    c->r[2] = c->r[3] & c->r[2];
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_80055E94; }
    c->mem_w8((c->r[16] + (uint32_t)330), (uint8_t)c->r[2]);
  L_80055E94:;
    c->r[2] = (uint32_t)32783u << 16;
    c->r[4] = c->r[2] + (uint32_t)-12472;
    c->r[3] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)-12472));
    c->r[2] = c->r[0] + (uint32_t)7;
    { int _t = (c->r[3] != c->r[2]); c->r[2] = c->r[0] + (uint32_t)3; if (_t) goto L_80055EB8; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)8));
    c->mem_w8((c->r[16] + (uint32_t)331), (uint8_t)c->r[2]); goto L_80055ECC;
  L_80055EB8:;
    c->mem_w8((c->r[16] + (uint32_t)331), (uint8_t)c->r[2]); goto L_80055ECC;
  L_80055EC0:;
    c->r[2] = c->r[3] | 2u;
    c->mem_w8((c->r[16] + (uint32_t)330), (uint8_t)c->r[2]);
    c->mem_w8((c->r[16] + (uint32_t)331), (uint8_t)c->r[0]);
  L_80055ECC:;
    c->r[2] = (uint32_t)32783u << 16;
    c->r[3] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)-12460));
    c->r[2] = c->r[3] & 16u;
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80055EF0; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)330));
    c->r[2] = c->r[2] | 4u; goto L_80055F08;
  L_80055EF0:;
    c->r[2] = c->r[3] & 64u;
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80055F0C; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)330));
    c->r[2] = c->r[2] | 8u;
  L_80055F08:;
    c->mem_w8((c->r[16] + (uint32_t)330), (uint8_t)c->r[2]);
  L_80055F0C:;
    { int _t = (c->r[5] == c->r[0]);  if (_t) goto L_80055F34; }
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)330));
    c->r[2] = c->r[3] & 2u;
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[3] & 1u; if (_t) goto L_80055F34; }
    c->mem_w8((c->r[16] + (uint32_t)327), (uint8_t)c->r[2]);
    c->r[31] = 0x80055F34u;
    c->r[4] = c->r[16] + c->r[0]; func_80055284(c);
  L_80055F34:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)330));
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_80055F48(Core* c) {
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)372));
    c->r[2] = c->r[2] & 48u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)32778u << 16; if (_t) goto L_80055F68; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)17794));
    c->mem_w16((c->r[4] + (uint32_t)74), (uint16_t)c->r[2]); goto L_80055F88;
  L_80055F68:;
    c->r[2] = (uint32_t)32778u << 16;
    c->r[3] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)109));
    c->r[2] = c->r[2] + (uint32_t)17788;
    c->r[3] = c->r[3] << 1;
    c->r[3] = c->r[3] + c->r[2];
    c->r[2] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)0));
    c->mem_w16((c->r[4] + (uint32_t)74), (uint16_t)c->r[2]);
  L_80055F88:;
    { int _t = (c->r[5] == c->r[0]);  if (_t) goto L_80055FB4; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)382));
    c->r[2] = c->r[2] & 384u;
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80055FB4; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)74));
    c->r[2] = c->r[2] + (uint32_t)6464;
    c->mem_w16((c->r[4] + (uint32_t)74), (uint16_t)c->r[2]);
  L_80055FB4:;
     return;
    return;
}

static void leaf_80055FBC(Core* c) {
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)372));
    c->r[2] = c->r[2] & 48u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)32778u << 16; if (_t) goto L_80055FD8; }
    c->r[6] = c->r[2] + (uint32_t)17780; goto L_80055FEC;
  L_80055FD8:;
    c->r[3] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)109));
    c->r[2] = (uint32_t)32778u << 16;
    c->r[2] = c->r[2] + (uint32_t)17756;
    c->r[3] = c->r[3] << 3;
    c->r[6] = c->r[3] + c->r[2];
  L_80055FEC:;
    c->r[9] = (uint32_t)c->mem_r16((c->r[6] + (uint32_t)0));
    c->r[11] = (uint32_t)c->mem_r16((c->r[6] + (uint32_t)2));
    c->r[7] = (uint32_t)c->mem_r16((c->r[6] + (uint32_t)6));
    c->r[10] = (uint32_t)c->mem_r16((c->r[6] + (uint32_t)4));
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)382));
    c->r[12] = c->r[7] + (uint32_t)-576;
    c->r[2] = c->r[2] & 48u;
    { int _t = (c->r[2] == c->r[0]); c->r[14] = c->r[7] + c->r[0]; if (_t) goto L_80056028; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)106));
    c->r[2] = c->r[2] & 3840u;
    c->r[2] = c->r[2] ^ 1280u;
    c->r[13] = (uint32_t)(c->r[2] < (uint32_t)1); goto L_8005602C;
  L_80056028:;
    c->r[13] = c->r[0] + c->r[0];
  L_8005602C:;
    c->r[2] = (uint32_t)32780u << 16;
    c->r[3] = c->r[2] + (uint32_t)-2040;
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)14));
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80056060; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)69));
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)3087; if (_t) goto L_80056060; }
    c->mem_w16((c->r[4] + (uint32_t)350), (uint16_t)c->r[2]);
    c->r[10] = c->r[2] + c->r[0]; goto L_8005610C;
  L_80056060:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)357));
    { int _t = (c->r[2] == c->r[0]); c->r[3] = c->r[9] << 16; if (_t) goto L_800560D0; }
    c->r[3] = (uint32_t)((int32_t)c->r[3] >> 16);
    c->r[2] = c->r[3] << 2;
    c->r[2] = c->r[2] + c->r[3];
    c->r[2] = c->r[2] << 2;
    c->r[2] = c->r[2] - c->r[3];
    c->r[9] = c->r[2] >> 4;
    c->r[2] = c->r[11] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[3] = c->r[2] << 1;
    c->r[11] = c->r[3] + c->r[2];
    c->r[3] = c->r[10] << 16;
    c->r[3] = (uint32_t)((int32_t)c->r[3] >> 16);
    c->r[2] = c->r[3] << 2;
    c->r[2] = c->r[2] + c->r[3];
    c->r[2] = c->r[2] << 2;
    c->r[2] = c->r[2] - c->r[3];
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[6] + (uint32_t)4));
    c->r[10] = c->r[2] >> 4;
    c->r[2] = c->r[3] << 2;
    c->r[2] = c->r[2] + c->r[3];
    c->r[2] = c->r[2] << 2;
    c->r[2] = c->r[2] - c->r[3];
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 4); goto L_80056104;
  L_800560D0:;
    c->r[3] = (uint32_t)(int8_t)c->mem_r8((c->r[4] + (uint32_t)331));
    c->r[2] = c->r[0] + (uint32_t)1;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = (uint32_t)((int32_t)c->r[3] < 2); if (_t) goto L_800560F8; }
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)2; if (_t) goto L_80056100; }
    { int _t = (c->r[3] == c->r[2]); c->r[2] = c->r[0] + (uint32_t)4367; if (_t) goto L_80056104; }
     goto L_80056100;
  L_800560F8:;
    c->r[2] = c->r[0] + (uint32_t)3087; goto L_80056104;
  L_80056100:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[6] + (uint32_t)4));
  L_80056104:;
    c->mem_w16((c->r[4] + (uint32_t)350), (uint16_t)c->r[2]);
  L_8005610C:;
    { int _t = (c->r[13] == c->r[0]); c->r[2] = c->r[9] << 16; if (_t) goto L_80056150; }
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    { int _t = ((int32_t)c->r[2] >= 0); c->r[9] = c->r[2] >> 3; if (_t) goto L_80056128; }
    c->r[2] = c->r[2] + (uint32_t)7;
    c->r[9] = c->r[2] >> 3;
  L_80056128:;
    c->r[2] = c->r[7] << 16;
    c->r[3] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = c->r[2] >> 31;
    c->r[3] = c->r[3] + c->r[2];
    c->r[7] = c->r[3] >> 1;
    c->r[2] = c->r[12] << 16;
    c->r[3] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = c->r[2] >> 31;
    c->r[3] = c->r[3] + c->r[2];
    c->r[12] = c->r[3] >> 1;
  L_80056150:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)325));
    { int _t = (c->r[2] == c->r[0]); c->r[3] = c->r[5] & 3u; if (_t) goto L_80056184; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)382));
    c->r[2] = c->r[2] & 96u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[11] << 16; if (_t) goto L_8005617C; }
    c->r[11] = c->r[2] >> 12; goto L_80056184;
  L_8005617C:;
    c->r[11] = c->r[2] >> 15;
    c->r[3] = c->r[5] & 3u;
  L_80056184:;
    c->r[2] = c->r[0] + (uint32_t)1;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = (uint32_t)((int32_t)c->r[3] < 2); if (_t) goto L_800564C4; }
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_800561A8; }
    { int _t = (c->r[3] == c->r[0]);  if (_t) goto L_800561BC; }
  L_800561A0:;
     return;
  L_800561A8:;
    c->r[2] = (uint32_t)((int32_t)c->r[3] < 4);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[5] & 8u; if (_t) goto L_800561A0; }
     goto L_800567C8;
  L_800561BC:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)95));
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[2] & 1u; if (_t) goto L_80056264; }
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80056264; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)95));
    c->r[2] = c->r[2] & 8u;
    { int _t = (c->r[2] != c->r[0]); c->mem_w8((c->r[4] + (uint32_t)329), (uint8_t)c->r[0]); if (_t) goto L_8005620C; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)68));
    c->r[2] = c->r[2] - c->r[7];
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    { int _t = ((int32_t)c->r[2] >= 0);  if (_t) goto L_80056870; }
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[0]); return;
  L_8005620C:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)68));
    c->r[3] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)68));
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 3329);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[3] + c->r[11]; if (_t) goto L_80056244; }
    c->r[2] = c->r[3] - c->r[12];
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 3328);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)3328; if (_t) goto L_800561A0; }
  L_8005623C:;
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[2]); return;
  L_80056244:;
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 3329);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)3328; if (_t) goto L_800561A0; }
     goto L_8005623C;
  L_80056264:;
    c->r[6] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)329));
    c->r[2] = c->r[6] & 2u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[6] & 1u; if (_t) goto L_80056298; }
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80056290; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)68));
    c->r[2] = c->r[2] + c->r[9];
  L_8005628C:;
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[2]);
  L_80056290:;
    c->mem_w8((c->r[4] + (uint32_t)329), (uint8_t)c->r[0]); return;
  L_80056298:;
    c->r[8] = c->r[6] & 255u;
    c->r[2] = c->r[0] + (uint32_t)5;
    { int _t = (c->r[8] != c->r[2]);  if (_t) goto L_8005633C; }
    { int _t = (c->r[13] == c->r[0]); c->r[2] = c->r[10] << 16; if (_t) goto L_80056308; }
    c->r[5] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = c->r[2] >> 31;
    c->r[2] = c->r[5] + c->r[2];
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 1);
    c->r[3] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)68));
    c->r[2] = c->r[0] - c->r[2];
    c->r[3] = c->r[3] + c->r[7];
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[3]);
    c->r[3] = c->r[3] << 16;
    c->r[3] = (uint32_t)((int32_t)c->r[3] >> 16);
    c->r[3] = (uint32_t)((int32_t)c->r[3] < (int32_t)c->r[2]);
    { int _t = (c->r[3] != c->r[0]);  if (_t) goto L_80056870; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)325));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[5] + c->r[0]; if (_t) goto L_80056290; }
    { int _t = ((int32_t)c->r[2] >= 0);  if (_t) goto L_80056300; }
    c->r[2] = c->r[2] + (uint32_t)3;
  L_80056300:;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 2); goto L_8005628C;
  L_80056308:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)68));
    c->r[2] = c->r[2] + c->r[7];
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    { int _t = ((int32_t)c->r[2] < 0);  if (_t) goto L_80056870; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)325));
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80056290; }
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[10]); goto L_80056290;
  L_8005633C:;
    c->r[5] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)68));
    c->r[3] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)68));
    { int _t = ((int32_t)c->r[5] <= 0); c->r[2] = c->r[9] << 16; if (_t) goto L_800563CC; }
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[5] < (int32_t)c->r[2]);
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80056364; }
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[9]); goto L_80056290;
  L_80056364:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)350));
    c->r[2] = (uint32_t)((int32_t)c->r[2] < (int32_t)c->r[5]);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[3] - c->r[12]; if (_t) goto L_800563A0; }
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)350));
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < (int32_t)c->r[3]);
    c->r[3] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)350));
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80056290; }
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[3]); goto L_80056290;
  L_800563A0:;
    c->r[2] = c->r[3] + c->r[11];
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)350));
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[3] = (uint32_t)((int32_t)c->r[3] < (int32_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)350));
    { int _t = (c->r[3] == c->r[0]);  if (_t) goto L_80056290; }
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[2]); goto L_80056290;
  L_800563CC:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)325));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_80056498; }
    { int _t = (c->r[8] != c->r[2]); c->r[2] = c->r[10] << 16; if (_t) goto L_80056488; }
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[3] = c->r[2] << 1;
    c->r[2] = c->r[3] + c->r[2];
    { int _t = ((int32_t)c->r[2] >= 0);  if (_t) goto L_800563FC; }
    c->r[2] = c->r[2] + (uint32_t)3;
  L_800563FC:;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 2);
    c->r[2] = c->r[0] - c->r[2];
    c->r[2] = (uint32_t)((int32_t)c->r[2] < (int32_t)c->r[5]);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[14] << 16; if (_t) goto L_80056488; }
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)68));
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    { int _t = ((int32_t)c->r[3] >= 0);  if (_t) goto L_80056424; }
    c->r[3] = c->r[0] - c->r[3];
  L_80056424:;
    cpu_div(c, c->r[3], c->r[2]);
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80056434; }
    rec_break(c, 7168u);
  L_80056434:;
    c->r[1] = c->r[0] + (uint32_t)-1;
    { int _t = (c->r[2] != c->r[1]); c->r[1] = (uint32_t)32768u << 16; if (_t) goto L_8005644C; }
    { int _t = (c->r[3] != c->r[1]);  if (_t) goto L_8005644C; }
    rec_break(c, 6144u);
  L_8005644C:;
    c->r[3] = c->lo;
    c->r[2] = c->r[6] & 1u;
    c->r[2] = c->r[2] | 4u;
    c->mem_w8((c->r[4] + (uint32_t)329), (uint8_t)c->r[2]);
    c->r[3] = c->r[3] + (uint32_t)-7;
    c->mem_w16((c->r[4] + (uint32_t)150), (uint16_t)c->r[3]);
    c->r[3] = c->r[3] << 16;
    { int _t = ((int32_t)c->r[3] > 0);  if (_t) goto L_80056474; }
    c->mem_w16((c->r[4] + (uint32_t)150), (uint16_t)c->r[8]);
  L_80056474:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)68));
    c->r[2] = c->r[2] + c->r[7];
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[2]); return;
  L_80056488:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)325));
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_800564B8; }
  L_80056498:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)382));
    c->r[2] = c->r[2] & 96u;
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_800564B8; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)68));
    c->r[2] = c->r[2] + c->r[11]; goto L_8005628C;
  L_800564B8:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)68));
    c->r[2] = c->r[2] + c->r[7]; goto L_8005628C;
  L_800564C4:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)95));
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[2] & 1u; if (_t) goto L_8005656C; }
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_8005656C; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)95));
    c->r[2] = c->r[2] & 8u;
    { int _t = (c->r[2] != c->r[0]); c->mem_w8((c->r[4] + (uint32_t)329), (uint8_t)c->r[3]); if (_t) goto L_80056514; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)68));
    c->r[2] = c->r[2] + c->r[7];
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    { int _t = ((int32_t)c->r[2] <= 0);  if (_t) goto L_80056870; }
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[0]); return;
  L_80056514:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)68));
    c->r[3] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)68));
    c->r[2] = (uint32_t)((int32_t)c->r[2] < -3328);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[3] + c->r[12]; if (_t) goto L_80056548; }
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < -3327);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)-3328; if (_t) goto L_800561A0; }
  L_80056540:;
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[2]); return;
  L_80056548:;
    c->r[2] = c->r[3] - c->r[11];
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < -3328);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)-3328; if (_t) goto L_800561A0; }
     goto L_80056540;
  L_8005656C:;
    c->r[6] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)329));
    c->r[2] = c->r[6] & 2u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[6] & 1u; if (_t) goto L_800565A4; }
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_8005659C; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)68));
    c->r[2] = c->r[2] - c->r[9];
  L_80056594:;
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[2]);
  L_80056598:;
    c->r[2] = c->r[0] + (uint32_t)1;
  L_8005659C:;
    c->mem_w8((c->r[4] + (uint32_t)329), (uint8_t)c->r[2]); return;
  L_800565A4:;
    c->r[8] = c->r[6] & 255u;
    c->r[2] = c->r[0] + (uint32_t)4;
    { int _t = (c->r[8] != c->r[2]);  if (_t) goto L_80056648; }
    { int _t = (c->r[13] == c->r[0]); c->r[2] = c->r[10] << 16; if (_t) goto L_80056614; }
    c->r[5] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = c->r[2] >> 31;
    c->r[2] = c->r[5] + c->r[2];
    c->r[3] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)68));
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 1);
    c->r[3] = c->r[3] - c->r[7];
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[3]);
    c->r[3] = c->r[3] << 16;
    c->r[3] = (uint32_t)((int32_t)c->r[3] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < (int32_t)c->r[3]);
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80056870; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)325));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_8005659C; }
    c->r[2] = c->r[0] - c->r[5];
    { int _t = ((int32_t)c->r[2] >= 0);  if (_t) goto L_8005660C; }
    c->r[2] = c->r[2] + (uint32_t)3;
  L_8005660C:;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 2); goto L_80056594;
  L_80056614:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)68));
    c->r[2] = c->r[2] - c->r[7];
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    { int _t = ((int32_t)c->r[2] > 0);  if (_t) goto L_80056870; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)325));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_8005659C; }
    c->r[2] = c->r[0] - c->r[10]; goto L_80056594;
  L_80056648:;
    c->r[5] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)68));
    c->r[3] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)68));
    { int _t = ((int32_t)c->r[5] >= 0); c->r[2] = c->r[9] << 16; if (_t) goto L_800566D0; }
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = c->r[0] - c->r[2];
    c->r[2] = (uint32_t)((int32_t)c->r[2] < (int32_t)c->r[5]);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] - c->r[9]; if (_t) goto L_80056594; }
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)350));
    c->r[2] = c->r[0] - c->r[2];
    c->r[2] = (uint32_t)((int32_t)c->r[5] < (int32_t)c->r[2]);
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_800566A0; }
    c->r[3] = c->r[3] + c->r[12];
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[3]);
    c->r[3] = c->r[3] << 16;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)350));
    c->r[3] = (uint32_t)((int32_t)c->r[3] >> 16);
    c->r[2] = c->r[0] - c->r[2]; goto L_800566B8;
  L_800566A0:;
    c->r[2] = c->r[3] - c->r[11];
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)350));
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[3] = c->r[0] - c->r[3];
  L_800566B8:;
    c->r[2] = (uint32_t)((int32_t)c->r[2] < (int32_t)c->r[3]);
    c->r[3] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)350));
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] - c->r[3]; if (_t) goto L_80056598; }
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[2]); goto L_80056598;
  L_800566D0:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)325));
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_8005679C; }
    { int _t = (c->r[8] != c->r[0]);  if (_t) goto L_80056790; }
    c->r[2] = c->r[10] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[3] = c->r[2] << 1;
    c->r[2] = c->r[3] + c->r[2];
    { int _t = ((int32_t)c->r[2] >= 0);  if (_t) goto L_80056704; }
    c->r[2] = c->r[2] + (uint32_t)3;
  L_80056704:;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 2);
    c->r[2] = (uint32_t)((int32_t)c->r[5] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[14] << 16; if (_t) goto L_8005678C; }
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)68));
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    { int _t = ((int32_t)c->r[3] >= 0);  if (_t) goto L_80056728; }
    c->r[3] = c->r[0] - c->r[3];
  L_80056728:;
    cpu_div(c, c->r[3], c->r[2]);
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80056738; }
    rec_break(c, 7168u);
  L_80056738:;
    c->r[1] = c->r[0] + (uint32_t)-1;
    { int _t = (c->r[2] != c->r[1]); c->r[1] = (uint32_t)32768u << 16; if (_t) goto L_80056750; }
    { int _t = (c->r[3] != c->r[1]);  if (_t) goto L_80056750; }
    rec_break(c, 6144u);
  L_80056750:;
    c->r[3] = c->lo;
    c->r[2] = c->r[6] & 1u;
    c->r[2] = c->r[2] | 4u;
    c->mem_w8((c->r[4] + (uint32_t)329), (uint8_t)c->r[2]);
    c->r[3] = c->r[3] + (uint32_t)-7;
    c->mem_w16((c->r[4] + (uint32_t)150), (uint16_t)c->r[3]);
    c->r[3] = c->r[3] << 16;
    { int _t = ((int32_t)c->r[3] > 0); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_80056778; }
    c->mem_w16((c->r[4] + (uint32_t)150), (uint16_t)c->r[2]);
  L_80056778:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)68));
    c->r[2] = c->r[2] - c->r[7];
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[2]); return;
  L_8005678C:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)325));
  L_80056790:;
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_800567BC; }
  L_8005679C:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)382));
    c->r[2] = c->r[2] & 96u;
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_800567BC; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)68));
    c->r[2] = c->r[2] - c->r[11]; goto L_80056594;
  L_800567BC:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)68));
    c->r[2] = c->r[2] - c->r[7]; goto L_80056594;
  L_800567C8:;
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80056800; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)382));
    c->r[2] = c->r[2] & 96u;
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80056800; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)325));
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80056800; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[6] + (uint32_t)6));
    c->r[7] = c->r[2] + (uint32_t)128; goto L_8005682C;
  L_80056800:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)95));
    c->r[3] = c->r[5] & 3u;
    c->r[2] = c->r[2] & 3u;
    { int _t = (c->r[2] == c->r[3]);  if (_t) goto L_8005682C; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)325));
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[5] & 1u; if (_t) goto L_8005682C; }
    c->r[7] = c->r[12] + c->r[0];
    c->mem_w8((c->r[4] + (uint32_t)329), (uint8_t)c->r[2]);
  L_8005682C:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)68));
    c->r[3] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)68));
    { int _t = ((int32_t)c->r[2] <= 0); c->r[2] = c->r[3] - c->r[7]; if (_t) goto L_80056854; }
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    { int _t = ((int32_t)c->r[2] > 0); c->r[2] = c->r[5] & 3u; if (_t) goto L_800561A0; }
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[0]); goto L_8005686C;
  L_80056854:;
    c->r[2] = c->r[3] + c->r[7];
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    { int _t = ((int32_t)c->r[2] < 0); c->r[2] = c->r[5] & 3u; if (_t) goto L_800561A0; }
    c->mem_w16((c->r[4] + (uint32_t)68), (uint16_t)c->r[0]);
  L_8005686C:;
    c->mem_w8((c->r[4] + (uint32_t)329), (uint8_t)c->r[2]);
  L_80056870:;
     return;
    return;
}

static void leaf_80056C00(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[31]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)41));
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)325));
    c->r[2] = c->r[2] | c->r[3];
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)32780u << 16; if (_t) goto L_80056C50; }
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)80));
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 5121);
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80056C44; }
    c->r[31] = 0x80056C44u;
     func_8005314C(c);
  L_80056C44:;
    c->mem_w16((c->r[16] + (uint32_t)80), (uint16_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)328), (uint8_t)c->r[0]); goto L_80056D34;
  L_80056C50:;
    c->r[3] = c->r[2] + (uint32_t)-2040;
    c->r[2] = (uint32_t)(int8_t)c->mem_r8((c->r[3] + (uint32_t)5));
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80056C84; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)64));
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_80056C84; }
    c->r[31] = 0x80056C7Cu;
    c->r[5] = c->r[0] + c->r[0]; func_80056D44(c);
     goto L_80056D34;
  L_80056C84:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)80));
    c->r[2] = c->r[2] + (uint32_t)2048;
    c->mem_w16((c->r[16] + (uint32_t)80), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 15873);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)15872; if (_t) goto L_80056CAC; }
    c->mem_w16((c->r[16] + (uint32_t)80), (uint16_t)c->r[2]);
  L_80056CAC:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)80));
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 7680);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_80056D20; }
    { int _t = (c->r[5] != c->r[0]); c->mem_w8((c->r[16] + (uint32_t)328), (uint8_t)c->r[2]); if (_t) goto L_80056D20; }
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)326));
    c->r[2] = c->r[0] + (uint32_t)4;
    { int _t = (c->r[3] == c->r[2]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_80056D20; }
    c->r[31] = 0x80056CE0u;
    c->r[5] = c->r[0] + c->r[0]; func_80056D44(c);
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)382));
    c->r[2] = c->r[2] & 64u;
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80056D10; }
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)80));
    { int _t = ((int32_t)c->r[2] >= 0);  if (_t) goto L_80056D08; }
    c->r[2] = c->r[2] + (uint32_t)3;
  L_80056D08:;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 2); goto L_80056D14;
  L_80056D10:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)80));
  L_80056D14:;
    c->mem_w16((c->r[16] + (uint32_t)74), (uint16_t)c->r[2]);
    c->mem_w16((c->r[16] + (uint32_t)80), (uint16_t)c->r[0]);
  L_80056D20:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)80));
    c->r[3] = c->mem_r32((c->r[16] + (uint32_t)48));
    c->r[2] = c->r[2] << 8;
    c->r[3] = c->r[3] + c->r[2];
    c->mem_w32((c->r[16] + (uint32_t)48), c->r[3]);
  L_80056D34:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_80056D44(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[17] = c->r[5] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[31]);
    c->mem_w8((c->r[16] + (uint32_t)385), (uint8_t)c->r[0]);
    c->mem_w16((c->r[16] + (uint32_t)386), (uint16_t)c->r[0]);
    c->r[31] = 0x80056D6Cu;
    c->mem_w8((c->r[16] + (uint32_t)362), (uint8_t)c->r[0]); func_80053D90(c);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)326));
    c->r[2] = c->r[2] & 3u;
    { int _t = (c->r[2] == c->r[0]); c->mem_w8((c->r[16] + (uint32_t)376), (uint8_t)c->r[0]); if (_t) goto L_80056DA4; }
    c->r[3] = c->r[0] + (uint32_t)1;
    c->r[2] = c->r[0] + (uint32_t)6;
    c->mem_w16((c->r[16] + (uint32_t)88), (uint16_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)325), (uint8_t)c->r[3]);
    c->mem_w8((c->r[16] + (uint32_t)41), (uint8_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)328), (uint8_t)c->r[0]);
    c->mem_w16((c->r[16] + (uint32_t)74), (uint16_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)5), (uint8_t)c->r[2]); goto L_80056DF0;
  L_80056DA4:;
    { int _t = (c->r[17] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)16; if (_t) goto L_80056DC4; }
    c->mem_w16((c->r[16] + (uint32_t)88), (uint16_t)c->r[0]);
    c->r[4] = c->r[16] + c->r[0];
    c->r[5] = c->r[0] + (uint32_t)20;
    c->r[31] = 0x80056DC0u;
    c->r[6] = c->r[0] + c->r[0]; func_80054D14(c);
    c->r[2] = c->r[0] + (uint32_t)16;
  L_80056DC4:;
    c->r[3] = c->r[0] + (uint32_t)1;
    c->mem_w8((c->r[16] + (uint32_t)359), (uint8_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)2;
    c->mem_w8((c->r[16] + (uint32_t)326), (uint8_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)325), (uint8_t)c->r[3]);
    c->mem_w8((c->r[16] + (uint32_t)41), (uint8_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)324), (uint8_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)328), (uint8_t)c->r[0]);
    c->mem_w16((c->r[16] + (uint32_t)74), (uint16_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)5), (uint8_t)c->r[2]);
    c->mem_w8((c->r[16] + (uint32_t)6), (uint8_t)c->r[3]);
  L_80056DF0:;
    c->mem_w8((c->r[16] + (uint32_t)7), (uint8_t)c->r[3]);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

static void leaf_80056E08(Core* c) {
    c->r[2] = (uint32_t)32783u << 16;
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)-12460));
    c->r[3] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)370));
    c->r[2] = c->r[2] & c->r[3];
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80056E50; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)359));
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w8((c->r[4] + (uint32_t)359), (uint8_t)c->r[2]);
    c->r[2] = c->r[2] & 255u;
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)255);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)254; if (_t) goto L_80056EC0; }
    c->mem_w8((c->r[4] + (uint32_t)359), (uint8_t)c->r[2]); return;
  L_80056E50:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)359));
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)11);
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80056EC0; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)382));
    c->r[2] = c->r[2] & 384u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)32778u << 16; if (_t) goto L_80056E84; }
    c->r[3] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)359));
    c->r[2] = c->r[2] + (uint32_t)17824; goto L_80056E90;
  L_80056E84:;
    c->r[2] = (uint32_t)32778u << 16;
    c->r[3] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)359));
    c->r[2] = c->r[2] + (uint32_t)17800;
  L_80056E90:;
    c->r[3] = c->r[3] << 1;
    c->r[3] = c->r[3] + c->r[2];
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)74));
    c->r[3] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)0));
    c->r[2] = c->r[2] + c->r[3];
    c->mem_w16((c->r[4] + (uint32_t)74), (uint16_t)c->r[2]);
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)74));
    c->r[2] = c->r[0] + (uint32_t)16;
    { int _t = ((int32_t)c->r[3] <= 0); c->mem_w8((c->r[4] + (uint32_t)359), (uint8_t)c->r[2]); if (_t) goto L_80056EC0; }
    c->mem_w16((c->r[4] + (uint32_t)74), (uint16_t)c->r[0]);
  L_80056EC0:;
     return;
    return;
}

static void leaf_80056F3C(Core* c) {
    c->r[7] = c->r[4] + c->r[0];
    c->r[2] = c->r[0] + (uint32_t)27;
    c->r[3] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)376));
    c->r[6] = c->mem_r32((c->r[7] + (uint32_t)260));
    c->r[5] = c->mem_r32((c->r[7] + (uint32_t)264));
    c->r[4] = c->r[2] - c->r[3];
    { int _t = ((int32_t)c->r[4] >= 0); c->r[2] = (uint32_t)((int32_t)c->r[4] < 7); if (_t) goto L_80056FC4; }
    c->mem_w16((c->r[5] + (uint32_t)10), (uint16_t)c->r[0]);
    c->mem_w16((c->r[6] + (uint32_t)10), (uint16_t)c->r[0]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)56));
    c->r[2] = c->r[2] + (uint32_t)-512;
    c->mem_w16((c->r[5] + (uint32_t)56), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    { int _t = ((int32_t)c->r[2] > 0); c->r[2] = c->r[0] + (uint32_t)2; if (_t) goto L_80056F8C; }
    c->mem_w16((c->r[5] + (uint32_t)56), (uint16_t)c->r[0]);
    c->mem_w8((c->r[7] + (uint32_t)377), (uint8_t)c->r[0]); goto L_80056F90;
  L_80056F8C:;
    c->mem_w8((c->r[7] + (uint32_t)377), (uint8_t)c->r[2]);
  L_80056F90:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)56));
    c->r[3] = c->r[2] + c->r[0];
    c->r[4] = c->r[2] + c->r[0];
    c->mem_w16((c->r[5] + (uint32_t)60), (uint16_t)c->r[2]);
    c->mem_w16((c->r[5] + (uint32_t)58), (uint16_t)c->r[3]);
    c->mem_w16((c->r[6] + (uint32_t)56), (uint16_t)c->r[4]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)56));
    c->mem_w16((c->r[6] + (uint32_t)58), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)56));
    c->mem_w16((c->r[6] + (uint32_t)60), (uint16_t)c->r[2]); return;
  L_80056FC4:;
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[4] << 3; if (_t) goto L_80057040; }
    c->r[2] = c->r[0] + (uint32_t)3456;
    c->mem_w16((c->r[5] + (uint32_t)10), (uint16_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)640;
    c->mem_w16((c->r[6] + (uint32_t)10), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)56));
    c->r[2] = c->r[2] + (uint32_t)512;
    c->mem_w16((c->r[5] + (uint32_t)56), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] & 65535u;
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)4096);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)4096; if (_t) goto L_80057000; }
    c->mem_w16((c->r[5] + (uint32_t)56), (uint16_t)c->r[2]);
  L_80057000:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)56));
    c->r[3] = c->r[2] + c->r[0];
    c->r[4] = c->r[2] + c->r[0];
    c->mem_w16((c->r[5] + (uint32_t)60), (uint16_t)c->r[2]);
    c->mem_w16((c->r[5] + (uint32_t)58), (uint16_t)c->r[3]);
    c->mem_w16((c->r[6] + (uint32_t)56), (uint16_t)c->r[4]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)56));
    c->mem_w16((c->r[6] + (uint32_t)58), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)56));
    c->mem_w16((c->r[6] + (uint32_t)60), (uint16_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)1;
    c->mem_w8((c->r[7] + (uint32_t)377), (uint8_t)c->r[2]); return;
  L_80057040:;
    c->r[2] = c->r[2] - c->r[4];
    c->r[3] = c->r[2] << 4;
    c->r[3] = c->r[3] - c->r[2];
    c->r[3] = c->r[3] + c->r[4];
    c->mem_w16((c->r[6] + (uint32_t)10), (uint16_t)c->r[3]);
    c->r[3] = c->r[0] - c->r[3];
    c->r[3] = c->r[3] & 4095u;
    c->r[2] = c->r[0] + (uint32_t)2;
    c->mem_w16((c->r[5] + (uint32_t)10), (uint16_t)c->r[3]);
    c->mem_w8((c->r[7] + (uint32_t)377), (uint8_t)c->r[2]); return;
    return;
}

static void leaf_8005706C(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)74));
    c->r[2] = c->r[2] + (uint32_t)864;
    c->mem_w16((c->r[16] + (uint32_t)74), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    c->r[3] = (uint32_t)((int32_t)c->r[2] >> 16);
    { int _t = ((int32_t)c->r[3] < 0); c->r[17] = c->r[5] + c->r[0]; if (_t) goto L_8005712C; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)382));
    c->r[2] = c->r[2] & 64u;
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_800570F8; }
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)70));
    c->r[2] = c->r[3] + (uint32_t)-20;
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)2);
    { int _t = (c->r[2] != c->r[0]); c->r[3] = c->r[3] & 255u; if (_t) goto L_800570E0; }
    c->r[2] = c->r[0] + (uint32_t)98;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = c->r[0] + (uint32_t)18; if (_t) goto L_800570E0; }
    { int _t = (c->r[3] != c->r[2]);  if (_t) goto L_80057104; }
  L_800570E0:;
    c->r[4] = c->r[16] + c->r[0];
    c->r[5] = c->r[0] + (uint32_t)202;
    c->r[31] = 0x800570F0u;
    c->r[6] = c->r[0] + (uint32_t)6; func_80054D14(c);
    c->mem_w8((c->r[16] + (uint32_t)376), (uint8_t)c->r[0]); goto L_80057104;
  L_800570F8:;
    c->mem_w8((c->r[16] + (uint32_t)376), (uint8_t)c->r[0]);
    c->r[31] = 0x80057104u;
    c->r[4] = c->r[16] + c->r[0]; func_80056EC8(c);
  L_80057104:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)7));
    c->r[3] = c->r[0] + (uint32_t)2;
    c->mem_w8((c->r[16] + (uint32_t)325), (uint8_t)c->r[3]);
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w8((c->r[16] + (uint32_t)7), (uint8_t)c->r[2]);
    c->r[2] = c->r[17] & 2u;
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_8005713C; }
    c->mem_w8((c->r[16] + (uint32_t)324), (uint8_t)c->r[2]); goto L_8005713C;
  L_8005712C:;
    c->r[2] = c->mem_r32((c->r[16] + (uint32_t)48));
    c->r[3] = c->r[3] << 8;
    c->r[2] = c->r[2] + c->r[3];
    c->mem_w32((c->r[16] + (uint32_t)48), c->r[2]);
  L_8005713C:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

static void leaf_800572EC(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
    c->r[18] = c->r[5] + c->r[0];
    c->r[5] = c->r[0] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[31]);
    c->r[31] = 0x80057310u;
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]); func_800541F4(c);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)41));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + c->r[0]; if (_t) goto L_80057484; }
    c->r[4] = c->r[16] + c->r[0];
    c->r[31] = 0x8005732Cu;
    c->r[5] = c->r[18] + c->r[0]; func_80057150(c);
    c->r[3] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)382));
    c->r[3] = c->r[3] & 64u;
    { int _t = (c->r[3] == c->r[0]); c->r[17] = c->r[2] + c->r[0]; if (_t) goto L_80057384; }
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)70));
    c->r[2] = c->r[3] + (uint32_t)-20;
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)2);
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_80057370; }
    c->r[3] = c->r[3] & 255u;
    c->r[2] = c->r[0] + (uint32_t)98;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = c->r[0] + (uint32_t)18; if (_t) goto L_80057370; }
    { int _t = (c->r[3] != c->r[2]);  if (_t) goto L_800573A8; }
  L_80057370:;
    c->r[5] = c->r[0] + (uint32_t)202;
    c->r[31] = 0x8005737Cu;
    c->r[6] = c->r[0] + (uint32_t)6; func_80054D14(c);
    c->mem_w8((c->r[16] + (uint32_t)376), (uint8_t)c->r[0]); goto L_800573A8;
  L_80057384:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)330));
    c->r[2] = c->r[2] & 8u;
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_800573A8; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)376));
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w8((c->r[16] + (uint32_t)376), (uint8_t)c->r[2]);
  L_800573A8:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)376));
    c->r[2] = c->r[2] + (uint32_t)1;
    c->r[3] = c->r[2] & 255u;
    c->mem_w8((c->r[16] + (uint32_t)376), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)(c->r[3] < (uint32_t)10);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)(c->r[3] < (uint32_t)255); if (_t) goto L_800573D0; }
    c->r[17] = c->r[0] + c->r[0]; goto L_800573DC;
  L_800573D0:;
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)255; if (_t) goto L_800573DC; }
    c->mem_w8((c->r[16] + (uint32_t)376), (uint8_t)c->r[2]);
  L_800573DC:;
    c->r[3] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)74));
    c->r[2] = c->r[17] << 16;
    c->r[3] = c->r[3] + (uint32_t)864;
    c->mem_w16((c->r[16] + (uint32_t)74), (uint16_t)c->r[3]);
    c->r[3] = c->r[3] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] < (int32_t)c->r[3]);
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80057400; }
    c->mem_w16((c->r[16] + (uint32_t)74), (uint16_t)c->r[17]);
  L_80057400:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)74));
    c->r[3] = c->mem_r32((c->r[16] + (uint32_t)48));
    c->r[2] = c->r[2] << 8;
    c->r[3] = c->r[3] + c->r[2];
    c->r[2] = c->r[0] + (uint32_t)2;
    { int _t = (c->r[18] != c->r[2]); c->mem_w32((c->r[16] + (uint32_t)48), c->r[3]); if (_t) goto L_8005744C; }
    c->r[4] = c->r[16] + c->r[0];
    c->r[6] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)98));
    c->r[5] = c->r[0] + c->r[0];
    c->r[6] = c->r[6] << 17;
    c->r[31] = 0x80057434u;
    c->r[6] = (uint32_t)((int32_t)c->r[6] >> 16); func_80049280(c);
    c->mem_w8((c->r[16] + (uint32_t)41), (uint8_t)c->r[2]);
    c->r[2] = c->r[2] & 255u;
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80057478; }
     goto L_80057484;
  L_8005744C:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)74));
    { int _t = ((int32_t)c->r[2] < 0); c->r[2] = c->r[18] & 2u; if (_t) goto L_80057478; }
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80057478; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)324));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_80057478; }
    c->mem_w8((c->r[16] + (uint32_t)324), (uint8_t)c->r[2]);
  L_80057478:;
    c->r[31] = 0x80057480u;
    c->r[4] = c->r[16] + c->r[0]; func_8005444C(c);
    c->r[2] = c->r[0] + c->r[0];
  L_80057484:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

static void leaf_8005749C(Core* c) {
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)326));
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_800574B0; }
    c->mem_w16((c->r[4] + (uint32_t)88), (uint16_t)c->r[0]);
  L_800574B0:;
    c->r[3] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)7));
    c->r[2] = c->r[0] + (uint32_t)1;
    c->mem_w8((c->r[4] + (uint32_t)376), (uint8_t)c->r[0]);
    c->mem_w8((c->r[4] + (uint32_t)359), (uint8_t)c->r[0]);
    c->mem_w8((c->r[4] + (uint32_t)325), (uint8_t)c->r[2]);
    c->mem_w8((c->r[4] + (uint32_t)41), (uint8_t)c->r[0]);
    c->mem_w16((c->r[4] + (uint32_t)106), (uint16_t)c->r[0]);
    c->mem_w8((c->r[4] + (uint32_t)324), (uint8_t)c->r[0]);
    c->mem_w8((c->r[4] + (uint32_t)328), (uint8_t)c->r[0]);
    c->r[3] = c->r[3] + (uint32_t)1;
    c->mem_w8((c->r[4] + (uint32_t)7), (uint8_t)c->r[3]); return;
    return;
}

static void leaf_800574E0(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-48;
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)40), c->r[18]);
    c->r[2] = c->r[0] + (uint32_t)1;
    c->mem_w32((c->r[29] + (uint32_t)44), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)36), c->r[17]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)7));
    c->r[17] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)382));
    { int _t = (c->r[3] == c->r[2]); c->r[18] = c->r[5] + c->r[0]; if (_t) goto L_80057598; }
    c->r[2] = (uint32_t)((int32_t)c->r[3] < 2);
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80057528; }
    { int _t = (c->r[3] == c->r[0]);  if (_t) goto L_8005753C; }
     goto L_80057A50;
  L_80057528:;
    c->r[2] = (uint32_t)((int32_t)c->r[3] < 4);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[17] & 32u; if (_t) goto L_80057A50; }
     goto L_800576D8;
  L_8005753C:;
    c->r[31] = 0x80057544u;
    c->r[4] = c->r[16] + c->r[0]; func_8005749C(c);
    c->r[2] = c->r[18] & 1u;
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_8005759C; }
    c->r[31] = 0x80057558u;
    c->r[5] = c->r[0] + (uint32_t)1; func_80055F48(c);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)357));
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80057578; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)74));
    c->r[2] = c->r[2] + (uint32_t)-1408;
    c->mem_w16((c->r[16] + (uint32_t)74), (uint16_t)c->r[2]);
  L_80057578:;
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)120));
    c->r[2] = c->r[0] + (uint32_t)6;
    { int _t = (c->r[3] != c->r[2]);  if (_t) goto L_80057598; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)74));
    c->r[2] = c->r[2] + (uint32_t)-4096;
    c->mem_w16((c->r[16] + (uint32_t)74), (uint16_t)c->r[2]);
  L_80057598:;
    c->r[4] = c->r[16] + c->r[0];
  L_8005759C:;
    c->r[31] = 0x800575A4u;
    c->r[5] = c->r[0] + c->r[0]; func_800541F4(c);
    c->r[31] = 0x800575ACu;
    c->r[4] = c->r[16] + c->r[0]; func_800543C0(c);
    c->r[2] = c->r[18] & 16u;
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[18] & 1u; if (_t) goto L_800575FC; }
    c->r[2] = (uint32_t)32780u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-1976));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[18] & 1u; if (_t) goto L_800575FC; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)97));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[18] & 1u; if (_t) goto L_800575FC; }
    c->r[2] = c->r[17] & 96u;
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_800575F8; }
    c->r[31] = 0x800575F0u;
    c->r[5] = c->r[18] + c->r[0]; func_8005706C(c);
    c->r[2] = c->r[18] & 1u; goto L_80057668;
  L_800575F8:;
    c->r[2] = c->r[18] & 1u;
  L_800575FC:;
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[0] + (uint32_t)864; if (_t) goto L_80057614; }
    c->r[2] = c->r[17] & 384u;
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80057614; }
    c->r[4] = c->r[0] + (uint32_t)288;
  L_80057614:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)74));
    c->r[2] = c->r[2] + c->r[4];
    c->mem_w16((c->r[16] + (uint32_t)74), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    { int _t = ((int32_t)c->r[2] < 0); c->r[3] = c->r[0] + (uint32_t)2; if (_t) goto L_80057650; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)7));
    c->mem_w8((c->r[16] + (uint32_t)325), (uint8_t)c->r[3]);
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w8((c->r[16] + (uint32_t)7), (uint8_t)c->r[2]);
    c->r[2] = c->r[18] & 2u;
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_80057650; }
    c->mem_w8((c->r[16] + (uint32_t)324), (uint8_t)c->r[2]);
  L_80057650:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)74));
    c->r[3] = c->mem_r32((c->r[16] + (uint32_t)48));
    c->r[2] = c->r[2] << 8;
    c->r[3] = c->r[3] + c->r[2];
    c->mem_w32((c->r[16] + (uint32_t)48), c->r[3]);
    c->r[2] = c->r[18] & 1u;
  L_80057668:;
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)32780u << 16; if (_t) goto L_8005767C; }
    c->r[31] = 0x80057678u;
    c->r[4] = c->r[16] + c->r[0]; func_80056E08(c);
    c->r[2] = (uint32_t)32780u << 16;
  L_8005767C:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-1976));
    c->r[2] = c->r[2] & 3u;
    { int _t = (c->r[2] == c->r[0]); c->r[7] = c->r[0] + (uint32_t)1; if (_t) goto L_80057A50; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)324));
    { int _t = (c->r[2] != c->r[7]); c->r[2] = (uint32_t)((int32_t)c->r[18] < 2); if (_t) goto L_800576A4; }
    c->mem_w8((c->r[16] + (uint32_t)324), (uint8_t)c->r[0]);
  L_800576A4:;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)32782u << 16; if (_t) goto L_800576C8; }
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)32360));
    c->r[3] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)370));
    c->r[2] = c->r[2] & c->r[3];
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80057908; }
  L_800576C8:;
    c->r[2] = c->r[0] + (uint32_t)2;
    c->mem_w8((c->r[16] + (uint32_t)7), (uint8_t)c->r[2]);
    c->mem_w8((c->r[16] + (uint32_t)325), (uint8_t)c->r[2]); goto L_80057A50;
  L_800576D8:;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[18] & 16u; if (_t) goto L_800576EC; }
    c->r[31] = 0x800576E8u;
    c->r[4] = c->r[16] + c->r[0]; func_80056F3C(c);
    c->r[2] = c->r[18] & 16u;
  L_800576EC:;
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)32780u << 16; if (_t) goto L_80057744; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-1976));
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80057744; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)97));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[17] & 64u; if (_t) goto L_80057744; }
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_8005772C; }
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)377));
    c->r[2] = c->r[0] + (uint32_t)1;
    { int _t = (c->r[3] != c->r[2]);  if (_t) goto L_80057748; }
  L_8005772C:;
    c->r[31] = 0x80057734u;
    c->r[5] = c->r[18] + c->r[0]; func_800572EC(c);
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_8005796C; }
     goto L_80057A50;
  L_80057744:;
    c->r[4] = c->r[16] + c->r[0];
  L_80057748:;
    c->r[31] = 0x80057750u;
    c->r[5] = c->r[0] + c->r[0]; func_800541F4(c);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)41));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[17] & 96u; if (_t) goto L_8005797C; }
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)8; if (_t) goto L_800577D8; }
    { int _t = (c->r[18] == c->r[2]); c->r[4] = c->r[0] + c->r[0]; if (_t) goto L_80057780; }
    c->r[5] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)88));
    c->r[31] = 0x8005777Cu;
    c->r[6] = c->r[0] + (uint32_t)64; func_800776F8(c);
    c->mem_w16((c->r[16] + (uint32_t)88), (uint16_t)c->r[2]);
  L_80057780:;
    c->r[2] = c->r[17] & 32u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[18] & 1u; if (_t) goto L_800577DC; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)330));
    c->r[2] = c->r[2] & 8u;
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_800577B0; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)376));
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w8((c->r[16] + (uint32_t)376), (uint8_t)c->r[2]);
  L_800577B0:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)376));
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w8((c->r[16] + (uint32_t)376), (uint8_t)c->r[2]);
    c->r[2] = c->r[2] & 255u;
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)255);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[18] & 1u; if (_t) goto L_800577DC; }
    c->r[2] = c->r[0] + (uint32_t)255;
    c->mem_w8((c->r[16] + (uint32_t)376), (uint8_t)c->r[2]);
  L_800577D8:;
    c->r[2] = c->r[18] & 1u;
  L_800577DC:;
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[0] + (uint32_t)864; if (_t) goto L_800577F8; }
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)70));
    c->r[2] = c->r[0] + (uint32_t)229;
    { int _t = (c->r[3] != c->r[2]);  if (_t) goto L_800577F8; }
    c->r[4] = c->r[0] + (uint32_t)1280;
  L_800577F8:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)74));
    c->r[2] = c->r[2] + c->r[4];
    c->mem_w16((c->r[16] + (uint32_t)74), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 15873);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)15872; if (_t) goto L_80057820; }
    c->mem_w16((c->r[16] + (uint32_t)74), (uint16_t)c->r[2]);
  L_80057820:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)74));
    c->r[3] = c->mem_r32((c->r[16] + (uint32_t)48));
    c->r[2] = c->r[2] << 8;
    c->r[3] = c->r[3] + c->r[2];
    c->r[2] = c->r[0] + (uint32_t)2;
    { int _t = (c->r[18] != c->r[2]); c->mem_w32((c->r[16] + (uint32_t)48), c->r[3]); if (_t) goto L_800578BC; }
    c->r[2] = (uint32_t)32780u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-1976));
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)32782u << 16; if (_t) goto L_8005788C; }
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)32360));
    c->r[3] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)370));
    c->r[2] = c->r[2] & c->r[3];
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[0] + (uint32_t)29; if (_t) goto L_8005788C; }
    c->r[5] = c->r[0] + c->r[0];
    c->r[6] = c->r[5] + c->r[0];
    c->r[2] = c->r[0] + (uint32_t)1;
    c->mem_w8((c->r[16] + (uint32_t)7), (uint8_t)c->r[0]);
    c->r[31] = 0x80057884u;
    c->mem_w8((c->r[16] + (uint32_t)325), (uint8_t)c->r[2]); func_80074590(c);
     goto L_80057A50;
  L_8005788C:;
    c->r[4] = c->r[16] + c->r[0];
    c->r[6] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)98));
    c->r[5] = c->r[0] + c->r[0];
    c->r[6] = c->r[6] << 17;
    c->r[31] = 0x800578A4u;
    c->r[6] = (uint32_t)((int32_t)c->r[6] >> 16); func_80049280(c);
    c->mem_w8((c->r[16] + (uint32_t)41), (uint8_t)c->r[2]);
    c->r[2] = c->r[2] & 255u;
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80057964; }
     goto L_80057A50;
  L_800578BC:;
    c->r[2] = (uint32_t)32780u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-1976));
    { int _t = (c->r[2] == c->r[0]); c->r[7] = c->r[0] + (uint32_t)1; if (_t) goto L_80057938; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)324));
    { int _t = (c->r[2] != c->r[7]); c->r[2] = (uint32_t)((int32_t)c->r[18] < 2); if (_t) goto L_800578E4; }
    c->mem_w8((c->r[16] + (uint32_t)324), (uint8_t)c->r[0]);
  L_800578E4:;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)32782u << 16; if (_t) goto L_80057964; }
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)32360));
    c->r[3] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)370));
    c->r[2] = c->r[2] & c->r[3];
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80057964; }
  L_80057908:;
    c->r[4] = c->r[16] + c->r[0];
    c->r[5] = c->r[0] + (uint32_t)20;
    c->r[6] = c->r[0] + (uint32_t)4;
    c->mem_w8((c->r[4] + (uint32_t)7), (uint8_t)c->r[0]);
    c->r[31] = 0x80057920u;
    c->mem_w8((c->r[4] + (uint32_t)325), (uint8_t)c->r[7]); func_80054D14(c);
    c->r[4] = c->r[0] + (uint32_t)29;
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x80057930u;
    c->r[6] = c->r[5] + c->r[0]; func_80074590(c);
     goto L_80057A50;
  L_80057938:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)74));
    { int _t = ((int32_t)c->r[2] < 0); c->r[2] = c->r[18] & 2u; if (_t) goto L_80057964; }
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80057964; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)324));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_80057964; }
    c->mem_w8((c->r[16] + (uint32_t)324), (uint8_t)c->r[2]);
  L_80057964:;
    c->r[31] = 0x8005796Cu;
    c->r[4] = c->r[16] + c->r[0]; func_8005444C(c);
  L_8005796C:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)41));
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80057A50; }
  L_8005797C:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)324));
    c->mem_w8((c->r[16] + (uint32_t)328), (uint8_t)c->r[0]);
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)2);
    { int _t = (c->r[2] == c->r[0]); c->mem_w16((c->r[16] + (uint32_t)80), (uint16_t)c->r[0]); if (_t) goto L_80057994; }
    c->mem_w8((c->r[16] + (uint32_t)324), (uint8_t)c->r[0]);
  L_80057994:;
    c->r[4] = c->mem_r32((c->r[16] + (uint32_t)44));
    c->mem_w8((c->r[16] + (uint32_t)325), (uint8_t)c->r[0]);
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[4]);
    c->r[2] = c->mem_r32((c->r[16] + (uint32_t)48));
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[2]);
    c->r[5] = c->mem_r32((c->r[16] + (uint32_t)52));
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[5]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)327));
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80057A00; }
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)72));
    c->r[2] = c->r[3] << 2;
    c->r[2] = c->r[2] + c->r[3];
    c->r[2] = c->r[2] << 11;
    c->r[2] = c->r[4] + c->r[2];
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[2]);
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)76));
    c->r[2] = c->r[3] << 2;
    c->r[2] = c->r[2] + c->r[3];
    c->r[2] = c->r[2] << 11;
    c->r[2] = c->r[5] + c->r[2]; goto L_80057A34;
  L_80057A00:;
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)72));
    c->r[2] = c->r[3] << 2;
    c->r[2] = c->r[2] + c->r[3];
    c->r[2] = c->r[2] << 11;
    c->r[2] = c->r[4] - c->r[2];
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[2]);
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)76));
    c->r[2] = c->r[3] << 2;
    c->r[2] = c->r[2] + c->r[3];
    c->r[2] = c->r[2] << 11;
    c->r[2] = c->r[5] - c->r[2];
  L_80057A34:;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[2]);
    c->r[4] = c->r[16] + c->r[0];
    c->r[5] = c->r[29] + (uint32_t)16;
    c->r[31] = 0x80057A48u;
    c->r[6] = c->r[0] + c->r[0]; func_800538E0(c);
    c->r[31] = 0x80057A50u;
    c->r[4] = c->r[16] + c->r[0]; func_8005314C(c);
  L_80057A50:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)44));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)40));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)36));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[29] = c->r[29] + (uint32_t)48; return;
    return;
}

static void leaf_80057A68(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[31]);
    c->r[31] = 0x80057A80u;
    c->r[5] = c->r[0] + c->r[0]; func_80024548(c);
    { int _t = (c->r[2] == c->r[0]); c->mem_w32((c->r[16] + (uint32_t)344), c->r[2]); if (_t) goto L_80057BF4; }
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w8((c->r[16] + (uint32_t)362), (uint8_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)385), (uint8_t)c->r[0]);
    c->r[3] = c->mem_r32((c->r[2] + (uint32_t)132));
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)327));
    c->r[7] = c->r[0] + (uint32_t)6;
    c->mem_w16((c->r[16] + (uint32_t)106), (uint16_t)c->r[0]);
    c->mem_w16((c->r[16] + (uint32_t)88), (uint16_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)325), (uint8_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)324), (uint8_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)6), (uint8_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)7), (uint8_t)c->r[0]);
    c->mem_w32((c->r[16] + (uint32_t)16), c->r[3]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)356));
    c->r[2] = c->r[2] | 2u;
    { int _t = (c->r[3] == c->r[7]); c->mem_w8((c->r[16] + (uint32_t)330), (uint8_t)c->r[2]); if (_t) goto L_80057BD0; }
    c->r[2] = (uint32_t)((int32_t)c->r[3] < 7);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)3; if (_t) goto L_80057B20; }
    { int _t = (c->r[3] == c->r[2]); c->r[2] = (uint32_t)((int32_t)c->r[3] < 4); if (_t) goto L_80057BC0; }
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_80057B00; }
    { int _t = (c->r[3] == c->r[2]); c->r[2] = c->r[0] + (uint32_t)2; if (_t) goto L_80057B88; }
    { int _t = (c->r[3] == c->r[2]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_80057BA4; }
     goto L_80057BF8;
  L_80057B00:;
    c->r[2] = c->r[0] + (uint32_t)4;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = c->r[0] + (uint32_t)20; if (_t) goto L_80057BD4; }
    c->r[2] = c->r[0] + (uint32_t)5;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = c->r[0] + (uint32_t)21; if (_t) goto L_80057BD4; }
    c->r[2] = c->r[0] + (uint32_t)1; goto L_80057BF8;
  L_80057B20:;
    c->r[2] = c->r[0] + (uint32_t)9;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = (uint32_t)((int32_t)c->r[3] < (int32_t)c->r[2]); if (_t) goto L_80057B88; }
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[0] + (uint32_t)5; if (_t) goto L_80057BA8; }
    c->r[2] = c->r[0] + (uint32_t)11;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = (uint32_t)((int32_t)c->r[3] < (int32_t)c->r[2]); if (_t) goto L_80057B8C; }
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)21; if (_t) goto L_80057BD4; }
    c->r[2] = c->r[0] + (uint32_t)129;
    { int _t = (c->r[3] != c->r[2]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_80057BF8; }
    c->r[4] = c->r[0] + (uint32_t)5;
    c->r[5] = c->r[0] + c->r[0];
    c->r[6] = c->r[5] + c->r[0];
    c->r[2] = c->r[0] + (uint32_t)24;
    c->mem_w8((c->r[16] + (uint32_t)5), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)356));
    c->r[3] = c->r[0] + (uint32_t)12;
    c->mem_w8((c->r[16] + (uint32_t)0), (uint8_t)c->r[7]);
    c->mem_w16((c->r[16] + (uint32_t)68), (uint16_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)6), (uint8_t)c->r[3]);
    c->r[2] = c->r[2] & 1u;
    c->mem_w8((c->r[16] + (uint32_t)356), (uint8_t)c->r[2]); goto L_80057BE4;
  L_80057B88:;
    c->r[4] = c->r[0] + (uint32_t)5;
  L_80057B8C:;
    c->r[5] = c->r[0] + c->r[0];
    c->r[6] = c->r[5] + c->r[0];
    c->r[2] = c->r[0] + (uint32_t)4;
    c->mem_w16((c->r[16] + (uint32_t)68), (uint16_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)5), (uint8_t)c->r[2]); goto L_80057BE4;
  L_80057BA4:;
    c->r[4] = c->r[0] + (uint32_t)5;
  L_80057BA8:;
    c->r[5] = c->r[0] + c->r[0];
    c->r[6] = c->r[5] + c->r[0];
    c->r[2] = c->r[0] + (uint32_t)7;
    c->mem_w16((c->r[16] + (uint32_t)68), (uint16_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)5), (uint8_t)c->r[2]); goto L_80057BE4;
  L_80057BC0:;
    c->r[2] = c->r[0] + (uint32_t)8;
    c->mem_w8((c->r[16] + (uint32_t)5), (uint8_t)c->r[2]);
    c->r[4] = c->r[0] + (uint32_t)5; goto L_80057BDC;
  L_80057BD0:;
    c->r[2] = c->r[0] + (uint32_t)22;
  L_80057BD4:;
    c->mem_w8((c->r[16] + (uint32_t)5), (uint8_t)c->r[2]);
    c->r[4] = c->r[0] + (uint32_t)4;
  L_80057BDC:;
    c->r[5] = c->r[0] + c->r[0];
    c->r[6] = c->r[5] + c->r[0];
  L_80057BE4:;
    c->r[31] = 0x80057BECu;
     func_80074590(c);
    c->r[2] = c->r[0] + (uint32_t)1; goto L_80057BF8;
  L_80057BF4:;
    c->r[2] = c->r[0] + c->r[0];
  L_80057BF8:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_80057C08(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[17] = c->r[5] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[31]);
    c->r[31] = 0x80057C28u;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]); func_80057A68(c);
    { int _t = (c->r[2] != c->r[0]); c->r[18] = c->r[0] + (uint32_t)2; if (_t) goto L_80057DA8; }
    { int _t = (c->r[17] == c->r[18]);  if (_t) goto L_80057DA8; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)41));
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80057CBC; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)382));
    c->r[2] = c->r[2] & 384u;
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80057CBC; }
    { int _t = (c->r[17] != c->r[0]);  if (_t) goto L_80057D24; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)362));
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80057CBC; }
    c->r[31] = 0x80057C7Cu;
     func_80055824(c);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)28; if (_t) goto L_80057CC4; }
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)325));
    c->mem_w16((c->r[16] + (uint32_t)88), (uint16_t)c->r[0]);
    { int _t = (c->r[3] != c->r[18]); c->mem_w8((c->r[16] + (uint32_t)5), (uint8_t)c->r[2]); if (_t) goto L_80057CB4; }
    c->r[2] = c->r[0] + (uint32_t)1;
    c->mem_w8((c->r[16] + (uint32_t)6), (uint8_t)c->r[2]);
    c->r[4] = c->r[16] + c->r[0];
    c->r[5] = c->r[0] + (uint32_t)229;
    c->r[31] = 0x80057CACu;
    c->r[6] = c->r[0] + (uint32_t)4; func_80054D14(c);
     goto L_80057DA8;
  L_80057CB4:;
    c->mem_w8((c->r[16] + (uint32_t)6), (uint8_t)c->r[0]); goto L_80057DA8;
  L_80057CBC:;
    { int _t = (c->r[17] != c->r[0]);  if (_t) goto L_80057D24; }
  L_80057CC4:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)362));
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_80057D24; }
    c->r[31] = 0x80057CDCu;
    c->r[5] = c->r[0] + c->r[0]; func_80055634(c);
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80057D24; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)330));
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)327));
    c->r[4] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)41));
    c->mem_w8((c->r[16] + (uint32_t)6), (uint8_t)c->r[0]);
    c->r[2] = c->r[2] & 14u;
    c->mem_w8((c->r[16] + (uint32_t)330), (uint8_t)c->r[2]);
    c->r[2] = c->r[2] | c->r[3];
    { int _t = (c->r[4] == c->r[0]); c->mem_w8((c->r[16] + (uint32_t)330), (uint8_t)c->r[2]); if (_t) goto L_80057D18; }
    c->r[2] = c->r[0] + (uint32_t)5;
    c->mem_w8((c->r[16] + (uint32_t)5), (uint8_t)c->r[2]);
    c->mem_w8((c->r[16] + (uint32_t)7), (uint8_t)c->r[0]); goto L_80057DA8;
  L_80057D18:;
    c->r[2] = c->r[0] + (uint32_t)6;
    c->mem_w8((c->r[16] + (uint32_t)5), (uint8_t)c->r[2]); goto L_80057DA8;
  L_80057D24:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)41));
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80057DA8; }
    c->r[31] = 0x80057D3Cu;
    c->r[4] = c->r[16] + c->r[0]; func_800532A0(c);
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80057DA8; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)330));
    c->r[4] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)327));
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)329));
    c->mem_w8((c->r[16] + (uint32_t)7), (uint8_t)c->r[0]);
    c->r[2] = c->r[2] & 14u;
    c->mem_w8((c->r[16] + (uint32_t)330), (uint8_t)c->r[2]);
    c->r[2] = c->r[2] | c->r[4];
    c->r[3] = c->r[3] & 2u;
    { int _t = (c->r[3] == c->r[0]); c->mem_w8((c->r[16] + (uint32_t)330), (uint8_t)c->r[2]); if (_t) goto L_80057D90; }
    c->r[4] = c->r[16] + c->r[0];
    c->r[5] = c->r[0] + (uint32_t)23;
    c->r[6] = c->r[0] + (uint32_t)1;
    c->r[2] = c->r[0] + (uint32_t)2;
    c->mem_w8((c->r[4] + (uint32_t)5), (uint8_t)c->r[2]);
    c->r[31] = 0x80057D88u;
    c->mem_w8((c->r[4] + (uint32_t)6), (uint8_t)c->r[2]); func_80054D14(c);
     goto L_80057DA8;
  L_80057D90:;
    c->r[4] = c->r[16] + c->r[0];
    c->r[5] = c->r[0] + (uint32_t)1;
    c->r[2] = c->r[5] + c->r[0];
    c->mem_w8((c->r[4] + (uint32_t)5), (uint8_t)c->r[2]);
    c->r[31] = 0x80057DA8u;
    c->mem_w8((c->r[4] + (uint32_t)6), (uint8_t)c->r[0]); func_80054E80(c);
  L_80057DA8:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

static void leaf_80059C60(Core* c) {
    c->r[2] = (uint32_t)32782u << 16;
    c->r[3] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)32384));
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[2] + (uint32_t)32384;
    { int _t = (c->r[3] == c->r[0]); c->mem_w32((c->r[29] + (uint32_t)20), c->r[31]); if (_t) goto L_80059D18; }
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)583));
    c->r[4] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)4));
    c->r[2] = c->r[2] + (uint32_t)1;
    { int _t = (c->r[4] == c->r[0]); c->mem_w8((c->r[3] + (uint32_t)583), (uint8_t)c->r[2]); if (_t) goto L_80059CA8; }
    c->r[2] = c->r[0] + (uint32_t)1;
    { int _t = (c->r[4] == c->r[2]);  if (_t) goto L_80059D10; }
     goto L_80059D18;
  L_80059CA8:;
    c->r[4] = c->r[16] + c->r[0];
    c->r[31] = 0x80059CB4u;
    c->r[5] = c->r[0] + (uint32_t)1; func_80058648(c);
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)566));
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)3);
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_80059CF8; }
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)382));
    c->r[2] = c->r[2] & 32768u;
    { int _t = (c->r[2] == c->r[0]); c->r[5] = c->r[0] + (uint32_t)4; if (_t) goto L_80059CFC; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)50));
    c->r[2] = c->r[2] + (uint32_t)70;
    c->mem_w16((c->r[16] + (uint32_t)50), (uint16_t)c->r[2]);
     goto L_80059CFC;
  L_80059CF8:;
    c->r[5] = c->r[0] + (uint32_t)2;
  L_80059CFC:;
    c->r[31] = 0x80059D04u;
    c->r[6] = c->r[0] + c->r[0]; func_80054D14(c);
    c->r[2] = c->r[0] + (uint32_t)1;
    c->mem_w8((c->r[16] + (uint32_t)1), (uint8_t)c->r[2]); goto L_80059D18;
  L_80059D10:;
    c->r[31] = 0x80059D18u;
    c->r[4] = c->r[16] + c->r[0]; func_80076D68(c);
  L_80059D18:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_8005A714(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[31]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)106));
    c->r[2] = c->r[2] << 16;
    c->r[3] = (uint32_t)((int32_t)c->r[2] >> 28);
    c->r[4] = c->r[3] & 7u;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 24);
    c->r[2] = c->r[2] & 15u;
    c->r[3] = c->r[2] + (uint32_t)-1;
    c->r[2] = (uint32_t)(c->r[3] < (uint32_t)10);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)32769u << 16; if (_t) goto L_8005A7E8; }
    c->r[2] = c->r[2] + (uint32_t)24356;
    c->r[3] = c->r[3] << 2;
    c->r[3] = c->r[3] + c->r[2];
    c->r[2] = c->mem_r32((c->r[3] + (uint32_t)0));
    {  switch (c->r[2]) { case 0x8005A76Cu: goto L_8005A76C; case 0x8005A7E8u: goto L_8005A7E8; case 0x8005A7BCu: goto L_8005A7BC; case 0x8005A794u: goto L_8005A794; default: rec_dispatch(c, c->r[2]); return; } }
  L_8005A76C:;
    c->r[4] = c->r[0] + (uint32_t)2;
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x8005A77Cu;
    c->r[6] = c->r[5] + c->r[0]; func_80074590(c);
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)70));
    c->r[2] = c->r[0] + (uint32_t)16;
    { int _t = (c->r[3] != c->r[2]); c->r[2] = c->r[0] + (uint32_t)9; if (_t) goto L_8005A8FC; }
    c->r[2] = c->r[0] + (uint32_t)11; goto L_8005A8FC;
  L_8005A794:;
    c->r[4] = c->r[0] + (uint32_t)144;
    c->r[5] = c->r[0] + (uint32_t)1;
    c->r[31] = 0x8005A7A4u;
    c->r[6] = c->r[0] + (uint32_t)-60; func_80074590(c);
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)70));
    c->r[2] = c->r[0] + (uint32_t)16;
    { int _t = (c->r[3] != c->r[2]); c->r[2] = c->r[0] + (uint32_t)9; if (_t) goto L_8005A8FC; }
    c->r[2] = c->r[0] + (uint32_t)11; goto L_8005A8FC;
  L_8005A7BC:;
    { int _t = (c->r[4] != c->r[0]); c->r[4] = c->r[0] + (uint32_t)138; if (_t) goto L_8005A7E8; }
    c->r[5] = c->r[0] + (uint32_t)1;
    c->r[31] = 0x8005A7D0u;
    c->r[6] = c->r[0] + (uint32_t)-60; func_80074590(c);
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)70));
    c->r[2] = c->r[0] + (uint32_t)16;
    { int _t = (c->r[3] != c->r[2]); c->r[2] = c->r[0] + (uint32_t)9; if (_t) goto L_8005A8FC; }
    c->r[2] = c->r[0] + (uint32_t)11; goto L_8005A8FC;
  L_8005A7E8:;
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)70));
    c->r[2] = c->r[0] + (uint32_t)10;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = (uint32_t)((int32_t)c->r[3] < 11); if (_t) goto L_8005A874; }
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)6; if (_t) goto L_8005A834; }
    { int _t = (c->r[3] == c->r[2]); c->r[2] = (uint32_t)((int32_t)c->r[3] < 7); if (_t) goto L_8005A874; }
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)4; if (_t) goto L_8005A820; }
    { int _t = (c->r[3] == c->r[2]); c->r[2] = c->r[0] + (uint32_t)9; if (_t) goto L_8005A874; }
    c->mem_w16((c->r[16] + (uint32_t)64), (uint16_t)c->r[2]); goto L_8005A900;
  L_8005A820:;
    c->r[2] = c->r[0] + (uint32_t)8;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = c->r[0] + (uint32_t)9; if (_t) goto L_8005A874; }
    c->mem_w16((c->r[16] + (uint32_t)64), (uint16_t)c->r[2]); goto L_8005A900;
  L_8005A834:;
    c->r[2] = c->r[0] + (uint32_t)14;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = (uint32_t)((int32_t)c->r[3] < 15); if (_t) goto L_8005A8B4; }
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)12; if (_t) goto L_8005A858; }
    { int _t = (c->r[3] == c->r[2]); c->r[2] = c->r[0] + (uint32_t)9; if (_t) goto L_8005A8B4; }
    c->mem_w16((c->r[16] + (uint32_t)64), (uint16_t)c->r[2]); goto L_8005A900;
  L_8005A858:;
    c->r[2] = c->r[0] + (uint32_t)16;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = c->r[0] + (uint32_t)224; if (_t) goto L_8005A8CC; }
    { int _t = (c->r[3] == c->r[2]); c->r[2] = c->r[0] + (uint32_t)9; if (_t) goto L_8005A8B4; }
    c->mem_w16((c->r[16] + (uint32_t)64), (uint16_t)c->r[2]); goto L_8005A900;
  L_8005A874:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)66));
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[0] + c->r[0]; if (_t) goto L_8005A88C; }
    c->r[5] = c->r[0] + (uint32_t)-3; goto L_8005A890;
  L_8005A88C:;
    c->r[5] = c->r[0] + (uint32_t)5;
  L_8005A890:;
    c->r[31] = 0x8005A898u;
    c->r[6] = c->r[4] + c->r[0]; func_80074590(c);
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)66));
    c->r[3] = c->r[0] + (uint32_t)9;
    c->mem_w16((c->r[16] + (uint32_t)64), (uint16_t)c->r[3]);
    c->r[2] = ~(c->r[0] | c->r[2]);
    c->r[2] = c->r[2] & 1u;
    c->mem_w16((c->r[16] + (uint32_t)66), (uint16_t)c->r[2]); goto L_8005A900;
  L_8005A8B4:;
    c->r[4] = c->r[0] + (uint32_t)1;
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x8005A8C4u;
    c->r[6] = c->r[5] + c->r[0]; func_80074590(c);
    c->r[2] = c->r[0] + (uint32_t)9; goto L_8005A8FC;
  L_8005A8CC:;
    c->r[4] = c->r[0] + (uint32_t)1;
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x8005A8DCu;
    c->r[6] = c->r[5] + c->r[0]; func_80074590(c);
    c->r[2] = c->r[0] + (uint32_t)11;
    c->mem_w16((c->r[16] + (uint32_t)64), (uint16_t)c->r[2]);
    c->r[4] = c->r[16] + c->r[0];
    c->r[5] = c->r[4] + (uint32_t)44;
    c->r[31] = 0x8005A8F4u;
    c->r[6] = c->r[0] + c->r[0]; func_800538E0(c);
     goto L_8005A900;
  L_8005A8FC:;
    c->mem_w16((c->r[16] + (uint32_t)64), (uint16_t)c->r[2]);
  L_8005A900:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_80062D8C(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-40;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
    c->r[18] = c->r[5] + c->r[0];
    c->r[2] = c->r[18] & 128u;
    c->mem_w32((c->r[29] + (uint32_t)36), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[20]);
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[19]);
    { int _t = (c->r[2] == c->r[0]); c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]); if (_t) goto L_80062E08; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)381));
    c->r[2] = c->r[2] & 2u;
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80062E08; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)80));
    c->r[2] = c->r[2] + (uint32_t)4;
    c->mem_w16((c->r[16] + (uint32_t)80), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 30);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)30; if (_t) goto L_80062DF4; }
    c->mem_w16((c->r[16] + (uint32_t)80), (uint16_t)c->r[2]);
  L_80062DF4:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)80));
    c->r[2] = c->r[0] - c->r[2];
    c->r[20] = c->r[2] << 8; goto L_80062E40;
  L_80062E08:;
    c->r[18] = c->r[18] & 3u;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)357));
    { int _t = (c->r[2] == c->r[0]); c->r[3] = c->r[18] + c->r[0]; if (_t) goto L_80062E20; }
    c->r[3] = c->r[18] + (uint32_t)3;
  L_80062E20:;
    c->r[2] = (uint32_t)32778u << 16;
    c->r[2] = c->r[2] + (uint32_t)18176;
    c->r[2] = c->r[3] + c->r[2];
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)0));
    c->r[2] = c->r[2] << 24;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[20] = c->r[0] - c->r[2];
  L_80062E40:;
    c->r[2] = c->r[20] << 16;
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)72));
    c->r[19] = (uint32_t)((int32_t)c->r[2] >> 16);
    { int64_t _p = (int64_t)(int32_t)c->r[3] * (int64_t)(int32_t)c->r[19]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[3] = c->lo;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)76));
    { int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[19]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[5] = c->lo;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)74));
    { int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[19]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[17] = c->r[0] + (uint32_t)37;
    c->r[2] = c->mem_r32((c->r[16] + (uint32_t)44));
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)86));
    c->r[2] = c->r[2] + c->r[3];
    c->mem_w32((c->r[16] + (uint32_t)44), c->r[2]);
    c->r[2] = c->mem_r32((c->r[16] + (uint32_t)52));
    c->r[3] = c->mem_r32((c->r[16] + (uint32_t)48));
    c->r[2] = c->r[2] + c->r[5];
    c->mem_w32((c->r[16] + (uint32_t)52), c->r[2]);
    c->r[6] = c->lo;
    c->r[3] = c->r[3] + c->r[6];
    c->r[31] = 0x80062EA4u;
    c->mem_w32((c->r[16] + (uint32_t)48), c->r[3]); func_80083F50(c);
    c->r[2] = c->r[2] << 3;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 12);
    c->r[3] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)46));
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)86));
    c->r[3] = c->r[3] + c->r[2];
    c->r[31] = 0x80062EC0u;
    c->mem_w16((c->r[16] + (uint32_t)46), (uint16_t)c->r[3]); func_80083E80(c);
    c->r[2] = c->r[2] << 3;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 12);
    c->r[3] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)54));
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)382));
    c->r[3] = c->r[3] - c->r[2];
    { int _t = ((int32_t)c->r[4] < 0); c->mem_w16((c->r[16] + (uint32_t)54), (uint16_t)c->r[3]); if (_t) goto L_80062EE0; }
    c->r[17] = c->r[0] + (uint32_t)74;
  L_80062EE0:;
    { int _t = (c->r[18] != c->r[0]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_80062EF4; }
    c->r[31] = 0x80062EF0u;
    c->r[5] = c->r[0] + (uint32_t)1; func_800541F4(c);
    c->r[4] = c->r[16] + c->r[0];
  L_80062EF4:;
    c->r[5] = c->r[17] + c->r[0];
    c->r[6] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)104));
    c->r[7] = c->r[5] + c->r[0];
    c->mem_w8((c->r[16] + (uint32_t)381), (uint8_t)c->r[0]);
    c->r[6] = c->r[0] - c->r[6];
    c->r[6] = c->r[6] << 16;
    c->r[31] = 0x80062F14u;
    c->r[6] = (uint32_t)((int32_t)c->r[6] >> 16); func_80046A44(c);
    c->r[17] = c->r[2] + c->r[0];
    { int _t = (c->r[17] == c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_80062F68; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)422));
    c->r[4] = c->r[16] + c->r[0];
    c->r[2] = c->r[2] >> 11;
    c->r[2] = c->r[2] & 3u;
    c->r[31] = 0x80062F38u;
    c->mem_w8((c->r[16] + (uint32_t)381), (uint8_t)c->r[2]); func_80048654(c);
    c->r[2] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)8064u << 16;
    c->r[5] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)416));
    c->r[2] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)418));
    c->r[4] = c->r[16] + c->r[0];
    c->r[2] = c->r[2] + (uint32_t)1024;
    c->mem_w16((c->r[16] + (uint32_t)320), (uint16_t)c->r[5]);
    c->r[31] = 0x80062F5Cu;
    c->mem_w16((c->r[16] + (uint32_t)322), (uint16_t)c->r[2]); func_80055284(c);
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)322));
    c->mem_w16((c->r[16] + (uint32_t)88), (uint16_t)c->r[2]);
  L_80062F68:;
    c->r[2] = c->r[0] + (uint32_t)1;
    { int _t = (c->r[17] == c->r[2]); c->r[2] = (uint32_t)((int32_t)c->r[17] < 2); if (_t) goto L_80062FFC; }
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80062F8C; }
    { int _t = (c->r[17] == c->r[0]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_80062FA0; }
     goto L_80063078;
  L_80062F8C:;
    c->r[2] = (uint32_t)((int32_t)c->r[17] < 4);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[20] << 16; if (_t) goto L_80063078; }
     goto L_80062FBC;
  L_80062FA0:;
    c->r[5] = c->r[0] + c->r[0];
    c->mem_w8((c->r[4] + (uint32_t)356), (uint8_t)c->r[0]);
    c->mem_w32((c->r[4] + (uint32_t)344), c->r[0]);
    c->r[31] = 0x80062FB4u;
    c->mem_w16((c->r[4] + (uint32_t)80), (uint16_t)c->r[0]); func_80056D44(c);
     goto L_80063078;
  L_80062FBC:;
    { int _t = ((int32_t)c->r[2] <= 0); c->r[2] = c->r[0] + (uint32_t)24; if (_t) goto L_80063044; }
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)70));
    { int _t = (c->r[3] == c->r[2]); c->r[4] = c->r[0] + (uint32_t)4; if (_t) goto L_80062FF0; }
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x80062FE0u;
    c->r[6] = c->r[5] + c->r[0]; func_80074590(c);
    c->r[4] = c->r[16] + c->r[0];
    c->r[5] = c->r[0] + (uint32_t)24;
    c->r[31] = 0x80062FF0u;
    c->r[6] = c->r[0] + c->r[0]; func_80054D14(c);
  L_80062FF0:;
    c->r[2] = c->r[0] + (uint32_t)2;
    c->mem_w8((c->r[16] + (uint32_t)6), (uint8_t)c->r[2]); goto L_80063078;
  L_80062FFC:;
    { int _t = ((int32_t)c->r[19] >= 0); c->r[2] = c->r[18] & 1u; if (_t) goto L_8006303C; }
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80063044; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)381));
    c->r[2] = c->r[2] & 1u;
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_80063028; }
    c->r[5] = c->r[0] + (uint32_t)66; goto L_8006302C;
  L_80063028:;
    c->r[5] = c->r[0] + (uint32_t)67;
  L_8006302C:;
    c->r[31] = 0x80063034u;
    c->r[6] = c->r[0] + c->r[0]; func_80054D14(c);
     goto L_80063044;
  L_8006303C:;
    { int _t = (c->r[19] != c->r[0]);  if (_t) goto L_80063078; }
  L_80063044:;
    c->r[31] = 0x8006304Cu;
    c->r[4] = c->r[16] + c->r[0]; func_8005444C(c);
    c->r[2] = c->r[2] & 1u;
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_80063078; }
    c->r[5] = c->r[0] + (uint32_t)2;
    c->r[6] = c->r[0] + (uint32_t)3;
    c->mem_w16((c->r[4] + (uint32_t)80), (uint16_t)c->r[0]);
    c->mem_w8((c->r[4] + (uint32_t)356), (uint8_t)c->r[0]);
    c->mem_w32((c->r[4] + (uint32_t)344), c->r[0]);
    c->mem_w8((c->r[4] + (uint32_t)5), (uint8_t)c->r[0]);
    c->r[31] = 0x80063078u;
    c->mem_w8((c->r[4] + (uint32_t)6), (uint8_t)c->r[0]); func_80054D14(c);
  L_80063078:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)36));
    c->r[20] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)40; return;
    return;
}

static void leaf_80067EF4(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[31]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)111));
    c->r[2] = (uint32_t)(c->r[3] < (uint32_t)109);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)32769u << 16; if (_t) goto L_80067F9C; }
    c->r[2] = c->r[2] + (uint32_t)26052;
    c->r[3] = c->r[3] << 2;
    c->r[3] = c->r[3] + c->r[2];
    c->r[2] = c->mem_r32((c->r[3] + (uint32_t)0));
    {  switch (c->r[2]) { case 0x80067F2Cu: goto L_80067F2C; case 0x80067F9Cu: goto L_80067F9C; case 0x80067F84u: goto L_80067F84; case 0x80067F48u: goto L_80067F48; case 0x80067F94u: goto L_80067F94; case 0x80067F40u: goto L_80067F40; case 0x80067F50u: goto L_80067F50; case 0x80067F58u: goto L_80067F58; case 0x80067F60u: goto L_80067F60; case 0x80067F68u: goto L_80067F68; case 0x80067F7Cu: goto L_80067F7C; case 0x80067F8Cu: goto L_80067F8C; default: rec_dispatch(c, c->r[2]); return; } }
  L_80067F2C:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)382));
    c->r[2] = c->r[2] & 32768u;
    c->r[2] = c->r[2] | 16u; goto L_80067F98;
  L_80067F40:;
    c->r[2] = c->r[0] + (uint32_t)266; goto L_80067F98;
  L_80067F48:;
    c->r[2] = c->r[0] + (uint32_t)65; goto L_80067F98;
  L_80067F50:;
    c->r[2] = c->r[0] + (uint32_t)1028; goto L_80067F98;
  L_80067F58:;
    c->r[2] = c->r[0] + (uint32_t)2053; goto L_80067F98;
  L_80067F60:;
    c->r[2] = c->r[0] + (uint32_t)4102; goto L_80067F98;
  L_80067F68:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)382));
    c->r[2] = c->r[2] & 32768u;
    c->r[2] = c->r[2] | 8199u; goto L_80067F98;
  L_80067F7C:;
    c->r[2] = c->r[0] + (uint32_t)16392; goto L_80067F98;
  L_80067F84:;
    c->r[2] = c->r[0] + (uint32_t)34; goto L_80067F98;
  L_80067F8C:;
    c->r[2] = c->r[0] + (uint32_t)521; goto L_80067F98;
  L_80067F94:;
    c->r[2] = c->r[0] + (uint32_t)131;
  L_80067F98:;
    c->mem_w16((c->r[4] + (uint32_t)382), (uint16_t)c->r[2]);
  L_80067F9C:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)372));
    c->r[2] = c->r[2] & 4u;
    { int _t = (c->r[2] == c->r[0]); c->r[3] = (uint32_t)32780u << 16; if (_t) goto L_80067FC0; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)382));
    c->r[2] = c->r[2] | 32768u;
    c->mem_w16((c->r[4] + (uint32_t)382), (uint16_t)c->r[2]);
  L_80067FC0:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)382));
    c->r[4] = (uint32_t)32783u << 16;
    c->r[4] = c->r[4] + (uint32_t)-12200;
    c->r[31] = 0x80067FD4u;
    c->mem_w16((c->r[3] + (uint32_t)-1890), (uint16_t)c->r[2]); func_80024E00(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_80067FE4(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[31]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)110));
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)3; if (_t) goto L_80068010; }
    c->r[3] = c->mem_r32((c->r[16] + (uint32_t)116));
    c->mem_w8((c->r[3] + (uint32_t)4), (uint8_t)c->r[2]);
  L_80068010:;
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)110));
    c->r[2] = c->r[0] + (uint32_t)40;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = c->r[0] + (uint32_t)97; if (_t) goto L_80068028; }
    { int _t = (c->r[3] != c->r[2]);  if (_t) goto L_8006803C; }
  L_80068028:;
    c->r[4] = c->r[16] + c->r[0];
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x80068038u;
    c->r[6] = c->r[5] + c->r[0]; func_8004BD04(c);
    c->mem_w32((c->r[16] + (uint32_t)116), c->r[2]);
  L_8006803C:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_80068214(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->r[2] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)566));
    c->r[2] = c->r[0] + (uint32_t)6;
    { int _t = (c->r[3] != c->r[2]); c->mem_w32((c->r[29] + (uint32_t)20), c->r[31]); if (_t) goto L_80068244; }
    c->r[31] = 0x8006823Cu;
    c->r[5] = c->r[0] + (uint32_t)1; func_800525D0(c);
     goto L_800682B4;
  L_80068244:;
    c->r[4] = c->r[0] + (uint32_t)27;
    c->r[31] = 0x80068250u;
    c->r[5] = c->r[0] + (uint32_t)-96; func_800310F4(c);
    c->r[4] = c->r[2] + c->r[0];
    { int _t = (c->r[4] == c->r[0]);  if (_t) goto L_800682B4; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)40));
    c->r[2] = c->r[2] | 128u;
    c->mem_w8((c->r[4] + (uint32_t)40), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)46));
    c->mem_w16((c->r[4] + (uint32_t)44), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)98));
    c->r[3] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)50));
    c->r[2] = c->r[2] << 1;
    c->r[3] = c->r[3] + c->r[2];
    c->mem_w16((c->r[4] + (uint32_t)46), (uint16_t)c->r[3]);
    c->r[3] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)54));
    c->r[2] = c->r[0] + (uint32_t)512;
    c->mem_w32((c->r[4] + (uint32_t)84), c->r[2]);
    c->mem_w32((c->r[4] + (uint32_t)88), c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)1;
    c->mem_w8((c->r[4] + (uint32_t)4), (uint8_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)4;
    c->mem_w32((c->r[4] + (uint32_t)80), c->r[0]);
    c->mem_w8((c->r[4] + (uint32_t)5), (uint8_t)c->r[2]);
    c->mem_w16((c->r[4] + (uint32_t)48), (uint16_t)c->r[3]);
  L_800682B4:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_800682C4(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->r[2] = (uint32_t)32780u << 16;
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[17] = c->r[2] + (uint32_t)-1936;
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)28));
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w8((c->r[16] + (uint32_t)108), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)28));
    c->mem_w8((c->r[16] + (uint32_t)374), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)29));
    c->mem_w8((c->r[16] + (uint32_t)109), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)31));
    c->mem_w8((c->r[16] + (uint32_t)111), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)30));
    c->mem_w8((c->r[16] + (uint32_t)110), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)17));
    c->mem_w8((c->r[16] + (uint32_t)372), (uint8_t)c->r[2]);
    c->r[2] = c->r[2] & 8u;
    { int _t = (c->r[2] == c->r[0]); c->r[18] = c->r[5] + c->r[0]; if (_t) goto L_8006833C; }
    c->r[2] = c->r[0] + (uint32_t)66;
    c->mem_w8((c->r[16] + (uint32_t)13), (uint8_t)c->r[2]);
  L_8006833C:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)372));
    c->r[2] = c->r[2] & 48u;
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80068360; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)13));
    c->r[2] = c->r[2] | 18u;
    c->mem_w8((c->r[16] + (uint32_t)13), (uint8_t)c->r[2]);
  L_80068360:;
    c->r[4] = c->r[16] + c->r[0];
    c->mem_w32((c->r[16] + (uint32_t)112), c->r[0]);
    c->r[31] = 0x80068370u;
    c->mem_w32((c->r[16] + (uint32_t)116), c->r[0]); func_80067DA8(c);
    c->r[31] = 0x80068378u;
    c->r[4] = c->r[16] + c->r[0]; func_80067EF4(c);
    { int _t = (c->r[18] != c->r[0]);  if (_t) goto L_800683A0; }
    c->r[31] = 0x80068388u;
    c->r[4] = c->r[16] + c->r[0]; func_80067FE4(c);
    c->r[3] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)373));
    c->r[2] = c->r[0] + (uint32_t)6;
    { int _t = (c->r[3] != c->r[2]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_800683A0; }
    c->r[31] = 0x800683A0u;
    c->r[5] = c->r[0] + (uint32_t)2; rec_dispatch(c, 0x8011740Cu);
  L_800683A0:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

static void leaf_8006CE74(Core* c) {
    c->r[3] = c->r[4] + c->r[0];
    c->r[4] = c->r[4] << 16;
    c->r[4] = (uint32_t)((int32_t)c->r[4] >> 16);
    { int _t = ((int32_t)c->r[4] < 0); c->r[2] = c->r[5] << 16; if (_t) goto L_8006CEA0; }
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < (int32_t)c->r[4]);
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_8006CEB8; }
    c->r[3] = c->r[5] + c->r[0]; goto L_8006CEB8;
  L_8006CEA0:;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = c->r[0] - c->r[2];
    c->r[2] = (uint32_t)((int32_t)c->r[4] < (int32_t)c->r[2]);
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_8006CEB8; }
    c->r[3] = c->r[0] - c->r[5];
  L_8006CEB8:;
    c->r[2] = c->r[3] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16); return;
    return;
}

static void leaf_8006CEC4(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-40;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[19]);
    c->r[19] = c->r[5] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
    c->r[18] = c->r[6] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[5] = (uint32_t)c->mem_r16((c->r[19] + (uint32_t)2));
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)2));
    c->r[17] = c->r[0] + c->r[0];
    c->r[4] = c->r[5] - c->r[2];
    c->r[2] = c->r[4] + (uint32_t)10;
    c->r[2] = c->r[2] & 65535u;
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)21);
    { int _t = (c->r[2] != c->r[0]); c->r[3] = c->r[4] + c->r[0]; if (_t) goto L_8006CF40; }
    { int _t = (c->r[18] == c->r[0]); c->r[4] = c->r[4] << 16; if (_t) goto L_8006CF28; }
    c->r[4] = (uint32_t)((int32_t)c->r[4] >> 16);
    c->r[5] = c->r[18] << 16;
    c->r[31] = 0x8006CF24u;
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16); func_8006CE74(c);
    c->r[3] = c->r[2] + c->r[0];
  L_8006CF28:;
    c->r[2] = c->r[3] << 16;
    c->r[3] = c->mem_r32((c->r[16] + (uint32_t)0));
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 3);
    c->r[3] = c->r[3] + c->r[2];
    c->mem_w32((c->r[16] + (uint32_t)0), c->r[3]); goto L_8006CF48;
  L_8006CF40:;
    c->mem_w16((c->r[16] + (uint32_t)2), (uint16_t)c->r[5]);
    c->r[17] = c->r[0] + (uint32_t)1;
  L_8006CF48:;
    c->r[5] = (uint32_t)c->mem_r16((c->r[19] + (uint32_t)6));
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)6));
    c->r[4] = c->r[5] - c->r[2];
    c->r[2] = c->r[4] + (uint32_t)10;
    c->r[2] = c->r[2] & 65535u;
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)21);
    { int _t = (c->r[2] != c->r[0]); c->r[3] = c->r[4] + c->r[0]; if (_t) goto L_8006CFA0; }
    { int _t = (c->r[18] == c->r[0]); c->r[4] = c->r[4] << 16; if (_t) goto L_8006CF88; }
    c->r[4] = (uint32_t)((int32_t)c->r[4] >> 16);
    c->r[5] = c->r[18] << 16;
    c->r[31] = 0x8006CF84u;
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16); func_8006CE74(c);
    c->r[3] = c->r[2] + c->r[0];
  L_8006CF88:;
    c->r[2] = c->r[3] << 16;
    c->r[3] = c->mem_r32((c->r[16] + (uint32_t)4));
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 3);
    c->r[3] = c->r[3] + c->r[2];
    c->mem_w32((c->r[16] + (uint32_t)4), c->r[3]); goto L_8006CFA8;
  L_8006CFA0:;
    c->mem_w16((c->r[16] + (uint32_t)6), (uint16_t)c->r[5]);
    c->r[17] = c->r[17] + (uint32_t)1;
  L_8006CFA8:;
    c->r[5] = (uint32_t)c->mem_r16((c->r[19] + (uint32_t)10));
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)10));
    c->r[4] = c->r[5] - c->r[2];
    c->r[2] = c->r[4] + (uint32_t)10;
    c->r[2] = c->r[2] & 65535u;
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)21);
    { int _t = (c->r[2] != c->r[0]); c->r[3] = c->r[4] + c->r[0]; if (_t) goto L_8006D000; }
    { int _t = (c->r[18] == c->r[0]); c->r[4] = c->r[4] << 16; if (_t) goto L_8006CFE8; }
    c->r[4] = (uint32_t)((int32_t)c->r[4] >> 16);
    c->r[5] = c->r[18] << 16;
    c->r[31] = 0x8006CFE4u;
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16); func_8006CE74(c);
    c->r[3] = c->r[2] + c->r[0];
  L_8006CFE8:;
    c->r[2] = c->r[3] << 16;
    c->r[3] = c->mem_r32((c->r[16] + (uint32_t)8));
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 3);
    c->r[3] = c->r[3] + c->r[2];
    c->mem_w32((c->r[16] + (uint32_t)8), c->r[3]); goto L_8006D008;
  L_8006D000:;
    c->mem_w16((c->r[16] + (uint32_t)10), (uint16_t)c->r[5]);
    c->r[17] = c->r[17] + (uint32_t)1;
  L_8006D008:;
    c->r[2] = c->r[17] ^ 3u;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)1);
    c->r[29] = c->r[29] + (uint32_t)40; return;
    return;
}

static void leaf_8006F138(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-40;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
    c->r[18] = c->r[4] + c->r[0];
    c->r[4] = c->r[18] + (uint32_t)84;
    c->r[5] = c->r[18] + (uint32_t)152;
    c->mem_w32((c->r[29] + (uint32_t)36), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[20]);
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[19]);
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[31] = 0x8006F164u;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]); func_80085480(c);
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[18] + (uint32_t)46));
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[18] + (uint32_t)50));
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[18] + (uint32_t)54));
    c->r[5] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)8));
    c->r[19] = c->r[0] + c->r[0];
    c->mem_w32((c->r[18] + (uint32_t)172), c->r[2]);
    c->mem_w32((c->r[18] + (uint32_t)176), c->r[3]);
    { int _t = (c->r[5] == c->r[0]); c->mem_w32((c->r[18] + (uint32_t)180), c->r[4]); if (_t) goto L_8006F2B0; }
    c->r[2] = (uint32_t)8064u << 16;
    c->r[20] = c->r[2] + (uint32_t)0;
    c->r[2] = c->r[19] << 2;
  L_8006F194:;
    c->r[2] = c->r[18] + c->r[2];
    c->r[17] = c->mem_r32((c->r[2] + (uint32_t)192));
    c->r[16] = (uint32_t)(int16_t)c->mem_r16((c->r[17] + (uint32_t)6));
    c->r[31] = 0x8006F1ACu;
    c->r[4] = c->r[20] + c->r[0]; func_80051794(c);
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[17] + (uint32_t)8));
    c->r[31] = 0x8006F1B8u;
    c->r[5] = c->r[20] + c->r[0]; func_80084D10(c);
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[17] + (uint32_t)12));
    c->r[31] = 0x8006F1C4u;
    c->r[5] = c->r[20] + c->r[0]; func_80085050(c);
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[17] + (uint32_t)10));
    c->r[31] = 0x8006F1D0u;
    c->r[5] = c->r[20] + c->r[0]; func_80084EB0(c);
    c->r[2] = c->r[0] + (uint32_t)-1;
    { int _t = (c->r[16] != c->r[2]); c->r[16] = c->r[16] << 2; if (_t) goto L_8006F230; }
    c->r[4] = c->r[18] + (uint32_t)152;
    c->r[5] = c->r[20] + c->r[0];
    c->r[31] = 0x8006F1ECu;
    c->r[6] = c->r[17] + (uint32_t)24; func_80084110(c);
    c->r[4] = c->r[17] + c->r[0];
    c->r[31] = 0x8006F1F8u;
    c->r[5] = c->r[17] + (uint32_t)44; func_80084220(c);
    c->r[2] = c->mem_r32((c->r[17] + (uint32_t)44));
    c->r[3] = c->mem_r32((c->r[18] + (uint32_t)172));
    c->r[2] = c->r[2] + c->r[3];
    c->mem_w32((c->r[17] + (uint32_t)44), c->r[2]);
    c->r[2] = c->mem_r32((c->r[17] + (uint32_t)48));
    c->r[3] = c->mem_r32((c->r[18] + (uint32_t)176));
    c->r[2] = c->r[2] + c->r[3];
    c->mem_w32((c->r[17] + (uint32_t)48), c->r[2]);
    c->r[2] = c->mem_r32((c->r[17] + (uint32_t)52));
    c->r[3] = c->mem_r32((c->r[18] + (uint32_t)180));
    c->r[2] = c->r[2] + c->r[3]; goto L_8006F298;
  L_8006F230:;
    c->r[5] = c->r[20] + c->r[0];
    c->r[16] = c->r[18] + c->r[16];
    c->r[4] = c->mem_r32((c->r[16] + (uint32_t)192));
    c->r[6] = c->r[17] + (uint32_t)24;
    c->r[31] = 0x8006F248u;
    c->r[4] = c->r[4] + (uint32_t)24; func_80084110(c);
    c->r[4] = c->r[17] + c->r[0];
    c->r[31] = 0x8006F254u;
    c->r[5] = c->r[17] + (uint32_t)44; func_80084220(c);
    c->r[3] = c->mem_r32((c->r[16] + (uint32_t)192));
    c->r[2] = c->mem_r32((c->r[17] + (uint32_t)44));
    c->r[3] = c->mem_r32((c->r[3] + (uint32_t)44));
    c->r[2] = c->r[2] + c->r[3];
    c->mem_w32((c->r[17] + (uint32_t)44), c->r[2]);
    c->r[3] = c->mem_r32((c->r[16] + (uint32_t)192));
    c->r[2] = c->mem_r32((c->r[17] + (uint32_t)48));
    c->r[3] = c->mem_r32((c->r[3] + (uint32_t)48));
    c->r[2] = c->r[2] + c->r[3];
    c->mem_w32((c->r[17] + (uint32_t)48), c->r[2]);
    c->r[3] = c->mem_r32((c->r[16] + (uint32_t)192));
    c->r[2] = c->mem_r32((c->r[17] + (uint32_t)52));
    c->r[3] = c->mem_r32((c->r[3] + (uint32_t)52));
    c->r[2] = c->r[2] + c->r[3];
  L_8006F298:;
    c->mem_w32((c->r[17] + (uint32_t)52), c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)8));
    c->r[19] = c->r[19] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[19] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[19] << 2; if (_t) goto L_8006F194; }
  L_8006F2B0:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)36));
    c->r[20] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)40; return;
    return;
}

static void leaf_800708B4(Core* c) {
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)532));
    c->mem_w8((c->r[2] + (uint32_t)6), (uint8_t)c->r[0]);
    c->mem_w8((c->r[2] + (uint32_t)3), (uint8_t)c->r[4]); return;
    return;
}

static void leaf_800716B4(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->r[5] = c->r[0] + (uint32_t)12;
    c->r[2] = (uint32_t)32783u << 16;
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[17] = c->r[2] + (uint32_t)-12456;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[7] = (uint32_t)32778u << 16;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[31]);
    c->r[6] = c->mem_r32((c->r[17] + (uint32_t)64));
    c->r[7] = c->r[7] + (uint32_t)19400;
    c->r[31] = 0x800716ECu;
    c->mem_w32((c->r[2] + (uint32_t)532), c->r[16]); func_800519E0(c);
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_80071754; }
    c->r[5] = (uint32_t)32770u << 16;
    c->r[5] = c->r[5] + (uint32_t)-18336;
    c->r[6] = (uint32_t)32778u << 16;
    c->r[6] = c->r[6] + (uint32_t)15568;
    c->r[3] = c->mem_r32((c->r[17] + (uint32_t)68));
    c->r[2] = c->r[0] + (uint32_t)15;
    c->mem_w8((c->r[16] + (uint32_t)11), (uint8_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)512;
    c->mem_w32((c->r[16] + (uint32_t)52), c->r[0]);
    c->mem_w32((c->r[16] + (uint32_t)48), c->r[0]);
    c->mem_w32((c->r[16] + (uint32_t)44), c->r[0]);
    c->mem_w16((c->r[16] + (uint32_t)188), (uint16_t)c->r[2]);
    c->mem_w16((c->r[16] + (uint32_t)186), (uint16_t)c->r[2]);
    c->mem_w16((c->r[16] + (uint32_t)184), (uint16_t)c->r[2]);
    c->mem_w16((c->r[16] + (uint32_t)88), (uint16_t)c->r[0]);
    c->mem_w16((c->r[16] + (uint32_t)86), (uint16_t)c->r[0]);
    c->mem_w16((c->r[16] + (uint32_t)84), (uint16_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)123), (uint8_t)c->r[0]);
    c->r[31] = 0x80071744u;
    c->mem_w32((c->r[16] + (uint32_t)60), c->r[3]); func_80040CDC(c);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)4));
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w8((c->r[16] + (uint32_t)4), (uint8_t)c->r[2]);
  L_80071754:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

static void leaf_800737F8(Core* c) {
    c->r[5] = c->r[4] + c->r[0];
    c->r[2] = (uint32_t)32780u << 16;
    c->r[4] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-1936));
    c->r[29] = c->r[29] + (uint32_t)-48;
    c->mem_w32((c->r[29] + (uint32_t)44), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)40), c->r[18]);
    c->mem_w32((c->r[29] + (uint32_t)36), c->r[17]);
    c->r[31] = 0x8007381Cu;
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[16]); func_80078798(c);
    c->r[18] = c->r[2] + c->r[0];
    c->r[31] = 0x80073828u;
    c->r[4] = c->r[18] + c->r[0]; func_80073750(c);
    c->r[17] = c->r[2] + c->r[0];
    { int _t = ((int32_t)c->r[17] >= 0); c->r[2] = c->r[0] + (uint32_t)16; if (_t) goto L_80073844; }
    c->r[2] = c->r[0] + (uint32_t)32;
    c->mem_w16((c->r[29] + (uint32_t)30), (uint16_t)c->r[2]);
    c->r[17] = c->r[0] - c->r[17]; goto L_80073848;
  L_80073844:;
    c->mem_w16((c->r[29] + (uint32_t)30), (uint16_t)c->r[2]);
  L_80073848:;
    c->r[2] = c->r[17] << 2;
    c->r[16] = c->r[0] + (uint32_t)160;
    c->r[16] = c->r[16] - c->r[2];
    c->r[4] = c->r[16] << 16;
    c->r[4] = (uint32_t)((int32_t)c->r[4] >> 16);
    c->r[5] = c->r[0] + (uint32_t)80;
    c->r[6] = c->r[0] + c->r[0];
    c->r[7] = c->r[18] + c->r[0];
    c->r[31] = 0x80073870u;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]); func_80079374(c);
    c->r[4] = c->r[29] + (uint32_t)24;
    c->r[5] = c->r[0] + (uint32_t)7;
    c->r[6] = c->r[0] + (uint32_t)1;
    c->r[7] = c->r[0] + c->r[0];
    c->r[2] = c->r[0] + (uint32_t)80;
    c->mem_w16((c->r[29] + (uint32_t)26), (uint16_t)c->r[2]);
    c->r[2] = c->r[17] << 3;
    c->mem_w16((c->r[29] + (uint32_t)24), (uint16_t)c->r[16]);
    c->r[31] = 0x80073898u;
    c->mem_w16((c->r[29] + (uint32_t)28), (uint16_t)c->r[2]); func_8005019C(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)44));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)40));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)36));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[29] = c->r[29] + (uint32_t)48; return;
    return;
}

static void leaf_800738B0(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-40;
    c->r[4] = c->r[0] + (uint32_t)16384;
    c->r[5] = c->r[0] + (uint32_t)188;
    c->r[6] = c->r[0] + (uint32_t)88;
    c->r[7] = c->r[0] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)36), c->r[31]);
    c->r[31] = 0x800738D0u;
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[16]); func_80033AFC(c);
    c->r[4] = c->r[0] + (uint32_t)8192;
    c->r[5] = c->r[0] + (uint32_t)188;
    c->r[6] = c->r[0] + (uint32_t)108;
    c->r[31] = 0x800738E4u;
    c->r[7] = c->r[0] + c->r[0]; func_80033AFC(c);
    c->r[16] = (uint32_t)32778u << 16;
    c->r[16] = c->r[16] + (uint32_t)10324;
    c->r[4] = c->mem_r32((c->r[16] + (uint32_t)12));
    c->r[31] = 0x800738F8u;
     func_80073750(c);
    c->r[4] = c->r[2] << 2;
    c->r[3] = c->r[0] + (uint32_t)160;
    c->r[3] = c->r[3] - c->r[4];
    c->r[4] = c->r[3] << 16;
    c->r[4] = (uint32_t)((int32_t)c->r[4] >> 16);
    c->r[5] = c->r[0] + (uint32_t)60;
    c->r[7] = c->mem_r32((c->r[16] + (uint32_t)12));
    c->r[6] = c->r[0] + c->r[0];
    c->mem_w16((c->r[29] + (uint32_t)24), (uint16_t)c->r[3]);
    c->r[3] = c->r[5] + c->r[0];
    c->r[2] = c->r[2] << 3;
    c->mem_w16((c->r[29] + (uint32_t)26), (uint16_t)c->r[3]);
    c->mem_w16((c->r[29] + (uint32_t)28), (uint16_t)c->r[2]);
    c->mem_w16((c->r[29] + (uint32_t)30), (uint16_t)c->r[3]);
    c->r[31] = 0x80073938u;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]); func_80079374(c);
    c->r[4] = c->r[0] + (uint32_t)128;
    c->r[5] = c->r[0] + (uint32_t)80;
    c->r[7] = c->mem_r32((c->r[16] + (uint32_t)16));
    c->r[6] = c->r[0] + c->r[0];
    c->r[31] = 0x80073950u;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]); func_80079374(c);
    c->r[4] = c->r[0] + (uint32_t)128;
    c->r[5] = c->r[0] + (uint32_t)100;
    c->r[7] = c->mem_r32((c->r[16] + (uint32_t)20));
    c->r[6] = c->r[0] + c->r[0];
    c->r[31] = 0x80073968u;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]); func_80079374(c);
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[29] + (uint32_t)24));
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 121);
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[29] + (uint32_t)24; if (_t) goto L_8007398C; }
    c->r[2] = c->r[0] + (uint32_t)120;
    c->mem_w16((c->r[29] + (uint32_t)24), (uint16_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)80;
    c->mem_w16((c->r[29] + (uint32_t)28), (uint16_t)c->r[2]);
  L_8007398C:;
    c->r[5] = c->r[0] + (uint32_t)7;
    c->r[6] = c->r[0] + (uint32_t)1;
    c->r[31] = 0x8007399Cu;
    c->r[7] = c->r[6] + c->r[0]; func_8005019C(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)36));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[29] = c->r[29] + (uint32_t)40; return;
    return;
}

static void leaf_8007413C(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->r[5] = c->r[0] + c->r[0];
    c->r[6] = c->r[0] + (uint32_t)5;
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[31]);
    c->r[31] = 0x8007415Cu;
    c->r[7] = c->r[0] + (uint32_t)4; func_80072DDC(c);
    c->r[4] = c->r[2] + c->r[0];
    { int _t = (c->r[4] == c->r[0]); c->r[2] = (uint32_t)32773u << 16; if (_t) goto L_800741C8; }
    c->r[2] = c->r[2] + (uint32_t)-14032;
    c->mem_w32((c->r[4] + (uint32_t)28), c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)46));
    c->mem_w16((c->r[4] + (uint32_t)46), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)132));
    c->r[2] = c->r[2] << 16;
    c->r[3] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = c->r[2] >> 31;
    c->r[3] = c->r[3] + c->r[2];
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)50));
    c->r[3] = (uint32_t)((int32_t)c->r[3] >> 1);
    c->r[2] = c->r[2] - c->r[3];
    c->mem_w16((c->r[4] + (uint32_t)50), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)54));
    c->mem_w16((c->r[4] + (uint32_t)54), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)40));
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)3));
    c->r[2] = c->r[2] | 128u;
    c->r[3] = c->r[3] + (uint32_t)43;
    c->mem_w8((c->r[4] + (uint32_t)40), (uint8_t)c->r[2]);
    c->mem_w8((c->r[4] + (uint32_t)3), (uint8_t)c->r[3]);
  L_800741C8:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[2] = c->r[4] + c->r[0];
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_80074B44(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->r[2] = (uint32_t)32780u << 16;
    c->r[4] = c->mem_r32((c->r[2] + (uint32_t)-4744));
    c->r[5] = c->r[0] + c->r[0];
    c->r[2] = (uint32_t)((int32_t)c->r[4] < 24);
    { int _t = (c->r[2] == c->r[0]); c->mem_w32((c->r[29] + (uint32_t)16), c->r[31]); if (_t) goto L_80074BA4; }
    c->r[6] = c->r[0] + (uint32_t)1;
    c->r[3] = (uint32_t)32780u << 16;
    c->r[3] = c->r[3] + (uint32_t)-7624;
    c->r[2] = c->r[4] << (c->r[6] & 31);
    c->r[2] = c->r[2] + c->r[4];
    c->r[2] = c->r[2] << 2;
    c->r[3] = c->r[2] + c->r[3];
  L_80074B7C:;
    c->r[2] = c->r[6] << (c->r[4] & 31);
    c->r[5] = c->r[5] | c->r[2];
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)1));
    c->r[4] = c->r[4] + (uint32_t)1;
    c->mem_w8((c->r[3] + (uint32_t)0), (uint8_t)c->r[0]);
    c->r[2] = c->r[2] & 192u;
    c->mem_w8((c->r[3] + (uint32_t)1), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)((int32_t)c->r[4] < 24);
    { int _t = (c->r[2] != c->r[0]); c->r[3] = c->r[3] + (uint32_t)12; if (_t) goto L_80074B7C; }
  L_80074BA4:;
    c->r[2] = (uint32_t)32780u << 16;
    c->r[4] = c->r[0] + c->r[0];
    c->r[31] = 0x80074BB4u;
    c->mem_w32((c->r[2] + (uint32_t)-7336), c->r[5]); func_80098F90(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_80074E48(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->r[2] = (uint32_t)32780u << 16;
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)-4736));
    c->r[2] = c->r[0] + (uint32_t)-1;
    { int _t = (c->r[3] == c->r[2]); c->mem_w32((c->r[29] + (uint32_t)16), c->r[31]); if (_t) goto L_80074EDC; }
    c->r[2] = (uint32_t)32780u << 16;
    c->r[2] = c->r[2] + (uint32_t)-7320;
    c->r[3] = c->r[3] << 3;
    c->r[3] = c->r[3] + c->r[2];
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[3] + (uint32_t)0));
    c->r[31] = 0x80074E7Cu;
     func_80091AF0(c);
    c->r[5] = (uint32_t)32780u << 16;
    c->r[2] = c->mem_r32((c->r[5] + (uint32_t)-4744));
    { int _t = ((int32_t)c->r[2] <= 0); c->r[4] = c->r[0] + c->r[0]; if (_t) goto L_80074EC0; }
    c->r[2] = (uint32_t)32780u << 16;
    c->r[3] = c->r[2] + (uint32_t)-7624;
  L_80074E98:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)1));
    c->r[4] = c->r[4] + (uint32_t)1;
    c->mem_w8((c->r[3] + (uint32_t)0), (uint8_t)c->r[0]);
    c->r[2] = c->r[2] & 192u;
    c->mem_w8((c->r[3] + (uint32_t)1), (uint8_t)c->r[2]);
    c->r[2] = c->mem_r32((c->r[5] + (uint32_t)-4744));
    c->r[2] = (uint32_t)((int32_t)c->r[4] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]); c->r[3] = c->r[3] + (uint32_t)12; if (_t) goto L_80074E98; }
  L_80074EC0:;
    c->r[4] = c->r[0] + c->r[0];
    c->r[2] = (uint32_t)32780u << 16;
    c->r[3] = (uint32_t)32780u << 16;
    c->mem_w32((c->r[2] + (uint32_t)-4744), c->r[0]);
    c->r[2] = c->r[0] + (uint32_t)-1;
    c->r[31] = 0x80074EDCu;
    c->mem_w16((c->r[3] + (uint32_t)-4736), (uint16_t)c->r[2]); func_800963A0(c);
  L_80074EDC:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_800753AC(Core* c) {
    c->r[2] = (uint32_t)32780u << 16;
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)-7928));
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[31]);
    c->r[31] = 0x800753C4u;
    c->r[5] = c->r[2] + c->r[5]; func_8001DC40(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_8007566C(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-40;
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[19]);
    c->r[19] = c->r[5] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
    c->r[18] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[20]);
    c->r[20] = (uint32_t)32792u << 16;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = (uint32_t)32778u << 16;
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)20350));
    c->r[20] = c->r[20] | 8192u;
    c->mem_w32((c->r[29] + (uint32_t)36), c->r[31]);
    { int _t = ((int32_t)c->r[4] < 0); c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]); if (_t) goto L_800756B4; }
    c->r[31] = 0x800756ACu;
     func_800963D0(c);
    c->r[2] = c->r[0] + (uint32_t)-1;
    c->mem_w16((c->r[16] + (uint32_t)20350), (uint16_t)c->r[2]);
  L_800756B4:;
    c->r[17] = c->r[0] + (uint32_t)10;
    c->r[2] = (uint32_t)32780u << 16;
    c->r[2] = c->r[2] + (uint32_t)-7320;
    c->r[16] = c->r[2] + (uint32_t)80;
  L_800756C4:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)6));
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_800756E4; }
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)0));
    c->r[31] = 0x800756E0u;
     func_8008DD7C(c);
    c->mem_w16((c->r[16] + (uint32_t)6), (uint16_t)c->r[0]);
  L_800756E4:;
    c->r[17] = c->r[17] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[17] < 14);
    { int _t = (c->r[2] != c->r[0]); c->r[16] = c->r[16] + (uint32_t)8; if (_t) goto L_800756C4; }
    c->r[2] = c->r[18] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 15);
    c->r[2] = c->r[2] + c->r[20];
    c->r[5] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)0));
    c->r[6] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)2));
    c->r[4] = c->r[19] + c->r[0];
    c->r[6] = c->r[6] - c->r[5];
    c->r[6] = c->r[6] << 11;
    c->r[2] = c->r[6] + (uint32_t)-2048;
    c->r[31] = 0x80075720u;
    c->r[16] = c->r[19] + c->r[2]; func_800753AC(c);
    c->r[4] = (uint32_t)32778u << 16;
    c->r[4] = c->r[4] + (uint32_t)20350;
    c->r[5] = c->mem_r32((c->r[16] + (uint32_t)0));
    c->r[6] = c->mem_r32((c->r[16] + (uint32_t)4));
    c->r[5] = c->r[19] + c->r[5];
    c->r[31] = 0x8007573Cu;
    c->r[6] = c->r[19] + c->r[6]; func_800753D4(c);
    c->r[2] = (uint32_t)32780u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-1936));
    c->r[3] = c->r[2] + (uint32_t)-5;
    c->r[2] = (uint32_t)(c->r[3] < (uint32_t)17);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)32769u << 16; if (_t) goto L_800757FC; }
    c->r[2] = c->r[2] + (uint32_t)27772;
    c->r[3] = c->r[3] << 2;
    c->r[3] = c->r[3] + c->r[2];
    c->r[2] = c->mem_r32((c->r[3] + (uint32_t)0));
    {  switch (c->r[2]) { case 0x800757D4u: goto L_800757D4; case 0x800757FCu: goto L_800757FC; case 0x80075774u: goto L_80075774; case 0x80075784u: goto L_80075784; case 0x80075794u: goto L_80075794; case 0x800757A4u: goto L_800757A4; case 0x800757B4u: goto L_800757B4; case 0x800757C4u: goto L_800757C4; case 0x800757E4u: goto L_800757E4; case 0x800757F4u: goto L_800757F4; default: rec_dispatch(c, c->r[2]); return; } }
  L_80075774:;
    c->r[31] = 0x8007577Cu;
     rec_dispatch(c, 0x80118E28u);
     goto L_800757FC;
  L_80075784:;
    c->r[31] = 0x8007578Cu;
     rec_dispatch(c, 0x80117988u);
     goto L_800757FC;
  L_80075794:;
    c->r[31] = 0x8007579Cu;
     rec_dispatch(c, 0x8011727Cu);
     goto L_800757FC;
  L_800757A4:;
    c->r[31] = 0x800757ACu;
     rec_dispatch(c, 0x80116FC8u);
     goto L_800757FC;
  L_800757B4:;
    c->r[31] = 0x800757BCu;
     rec_dispatch(c, 0x801174ACu);
     goto L_800757FC;
  L_800757C4:;
    c->r[31] = 0x800757CCu;
     rec_dispatch(c, 0x8011A428u);
     goto L_800757FC;
  L_800757D4:;
    c->r[31] = 0x800757DCu;
     rec_dispatch(c, 0x8013AC40u);
     goto L_800757FC;
  L_800757E4:;
    c->r[31] = 0x800757ECu;
     rec_dispatch(c, 0x801174B0u);
     goto L_800757FC;
  L_800757F4:;
    c->r[31] = 0x800757FCu;
     rec_dispatch(c, 0x80110774u);
  L_800757FC:;
    c->r[2] = c->mem_r32((c->r[16] + (uint32_t)4));
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)36));
    c->r[20] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[2] = c->r[19] + c->r[2];
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[29] = c->r[29] + (uint32_t)40; return;
    return;
}

static void leaf_80075D58(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->r[5] = (uint32_t)14563u << 16;
    c->r[4] = (uint32_t)32784u << 16;
    c->r[4] = c->r[4] + (uint32_t)-20128;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[31]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)4));
    c->r[5] = c->r[5] | 36409u;
    c->r[2] = c->r[3] << 15;
    c->r[2] = c->r[2] - c->r[3];
    { int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[5]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[4] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)3));
    c->r[5] = (uint32_t)32780u << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 31);
    c->r[6] = c->hi;
    c->r[3] = (uint32_t)((int32_t)c->r[6] >> 1);
    c->r[3] = c->r[3] - c->r[2];
    { int _t = (c->r[4] != c->r[0]); c->mem_w16((c->r[5] + (uint32_t)-7648), (uint16_t)c->r[3]); if (_t) goto L_80075DC8; }
    c->r[2] = (uint32_t)32780u << 16;
    c->r[3] = c->r[0] + (uint32_t)128;
    c->mem_w8((c->r[2] + (uint32_t)-4740), (uint8_t)c->r[3]);
    c->r[2] = c->r[2] + (uint32_t)-4740;
    c->mem_w8((c->r[2] + (uint32_t)1), (uint8_t)c->r[0]);
    c->mem_w8((c->r[2] + (uint32_t)2), (uint8_t)c->r[3]);
    c->r[31] = 0x80075DC0u;
    c->mem_w8((c->r[2] + (uint32_t)3), (uint8_t)c->r[0]); func_80096390(c);
    c->r[4] = (uint32_t)32780u << 16; goto L_80075DEC;
  L_80075DC8:;
    c->r[2] = (uint32_t)32780u << 16;
    c->r[3] = c->r[0] + (uint32_t)64;
    c->mem_w8((c->r[2] + (uint32_t)-4740), (uint8_t)c->r[3]);
    c->r[2] = c->r[2] + (uint32_t)-4740;
    c->mem_w8((c->r[2] + (uint32_t)1), (uint8_t)c->r[3]);
    c->mem_w8((c->r[2] + (uint32_t)2), (uint8_t)c->r[3]);
    c->r[31] = 0x80075DE8u;
    c->mem_w8((c->r[2] + (uint32_t)3), (uint8_t)c->r[3]); func_80096380(c);
    c->r[4] = (uint32_t)32780u << 16;
  L_80075DEC:;
    c->r[31] = 0x80075DF4u;
    c->r[4] = c->r[4] + (uint32_t)-4740; func_80089F68(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_80075FF8(Core* c) {
    c->r[12] = c->r[4] + c->r[0];
    c->r[7] = (uint32_t)255u << 16;
    c->r[7] = c->r[7] | 65535u;
    c->r[4] = (uint32_t)32767u << 16;
    c->r[4] = c->r[4] | 65535u;
    c->r[8] = c->r[6] + c->r[0];
    c->r[3] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)0));
    c->r[2] = c->mem_r32((c->r[12] + (uint32_t)60));
    c->r[3] = c->r[3] << 2;
    c->r[3] = c->r[3] + c->r[2];
    c->r[2] = c->mem_r32((c->r[12] + (uint32_t)56));
    c->r[13] = c->mem_r32((c->r[3] + (uint32_t)0));
    c->r[2] = c->r[2] & c->r[4];
    c->r[3] = (uint32_t)((int32_t)c->r[13] >> 24);
    c->mem_w32((c->r[12] + (uint32_t)56), c->r[2]);
    c->r[2] = c->mem_r32((c->r[12] + (uint32_t)60));
    c->r[13] = c->r[13] & c->r[7];
    c->r[7] = c->r[2] + c->r[13];
    c->r[2] = c->r[3] & 64u;
    { int _t = (c->r[2] == c->r[0]); c->mem_w8((c->r[12] + (uint32_t)8), (uint8_t)c->r[3]); if (_t) goto L_800764CC; }
    c->r[2] = c->r[3] & 128u;
    { int _t = (c->r[2] == c->r[0]); c->r[5] = (uint32_t)8064u << 16; if (_t) goto L_80076198; }
    c->r[4] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[7] = c->r[7] + (uint32_t)1;
    c->r[4] = c->r[4] << 24;
    c->r[4] = (uint32_t)((int32_t)c->r[4] >> 20);
    c->mem_w16((c->r[5] + (uint32_t)192), (uint16_t)c->r[4]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[2] = c->r[2] >> 4;
    c->r[4] = c->r[4] | c->r[2];
    c->mem_w16((c->r[5] + (uint32_t)192), (uint16_t)c->r[4]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[7] = c->r[7] + (uint32_t)1;
    c->r[5] = c->r[5] + (uint32_t)192;
    c->r[2] = c->r[2] << 28;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 20);
    c->mem_w16((c->r[5] + (uint32_t)2), (uint16_t)c->r[2]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[7] = c->r[7] + (uint32_t)1;
    c->r[2] = c->r[2] | c->r[3];
    c->mem_w16((c->r[5] + (uint32_t)2), (uint16_t)c->r[2]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[7] = c->r[7] + (uint32_t)1;
    c->r[3] = c->r[3] << 24;
    c->r[3] = (uint32_t)((int32_t)c->r[3] >> 20);
    c->mem_w16((c->r[5] + (uint32_t)4), (uint16_t)c->r[3]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[2] = c->r[2] >> 4;
    c->r[3] = c->r[3] | c->r[2];
    c->mem_w16((c->r[5] + (uint32_t)4), (uint16_t)c->r[3]);
    c->r[3] = c->r[6] << 16;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[12] + (uint32_t)136));
    c->r[3] = (uint32_t)((int32_t)c->r[3] >> 16);
    c->r[4] = c->r[4] - c->r[2];
    cpu_div(c, c->r[4], c->r[3]);
    { int _t = (c->r[3] != c->r[0]);  if (_t) goto L_800760F0; }
    rec_break(c, 7168u);
  L_800760F0:;
    c->r[1] = c->r[0] + (uint32_t)-1;
    { int _t = (c->r[3] != c->r[1]); c->r[1] = (uint32_t)32768u << 16; if (_t) goto L_80076108; }
    { int _t = (c->r[4] != c->r[1]);  if (_t) goto L_80076108; }
    rec_break(c, 6144u);
  L_80076108:;
    c->r[4] = c->lo;
    c->mem_w16((c->r[12] + (uint32_t)144), (uint16_t)c->r[4]);
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[5] + (uint32_t)2));
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[12] + (uint32_t)138));
    c->r[2] = c->r[2] - c->r[4];
    cpu_div(c, c->r[2], c->r[3]);
    { int _t = (c->r[3] != c->r[0]);  if (_t) goto L_80076134; }
    rec_break(c, 7168u);
  L_80076134:;
    c->r[1] = c->r[0] + (uint32_t)-1;
    { int _t = (c->r[3] != c->r[1]); c->r[1] = (uint32_t)32768u << 16; if (_t) goto L_8007614C; }
    { int _t = (c->r[2] != c->r[1]);  if (_t) goto L_8007614C; }
    rec_break(c, 6144u);
  L_8007614C:;
    c->r[2] = c->lo;
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[12] + (uint32_t)140));
    c->mem_w16((c->r[12] + (uint32_t)146), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[5] + (uint32_t)4));
    c->r[2] = c->r[2] - c->r[4];
    cpu_div(c, c->r[2], c->r[3]);
    { int _t = (c->r[3] != c->r[0]);  if (_t) goto L_80076174; }
    rec_break(c, 7168u);
  L_80076174:;
    c->r[1] = c->r[0] + (uint32_t)-1;
    { int _t = (c->r[3] != c->r[1]); c->r[1] = (uint32_t)32768u << 16; if (_t) goto L_8007618C; }
    { int _t = (c->r[2] != c->r[1]);  if (_t) goto L_8007618C; }
    rec_break(c, 6144u);
  L_8007618C:;
    c->r[2] = c->lo;
    c->r[7] = c->r[7] + (uint32_t)5;
    c->mem_w16((c->r[12] + (uint32_t)148), (uint16_t)c->r[2]);
  L_80076198:;
    c->r[11] = c->r[0] + c->r[0];
    c->r[2] = (uint32_t)c->mem_r8((c->r[12] + (uint32_t)8));
    c->r[3] = (uint32_t)c->mem_r8((c->r[12] + (uint32_t)9));
    c->r[2] = c->r[2] & 63u;
    { int _t = (c->r[3] == c->r[0]); c->mem_w8((c->r[12] + (uint32_t)8), (uint8_t)c->r[2]); if (_t) goto L_800768FC; }
    c->r[10] = (uint32_t)8064u << 16;
    c->r[8] = c->r[10] + (uint32_t)192;
    c->r[2] = c->r[6] << 16;
    c->r[9] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[5] = c->r[12] + c->r[0];
  L_800761C4:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[12] + (uint32_t)8));
    c->r[2] = (uint32_t)((int32_t)c->r[11] < (int32_t)c->r[2]);
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_800768FC; }
    c->r[3] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[7] = c->r[7] + (uint32_t)1;
    c->r[3] = c->r[3] << 4;
    c->mem_w16((c->r[10] + (uint32_t)192), (uint16_t)c->r[3]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[2] = c->r[2] >> 4;
    c->r[3] = c->r[3] | c->r[2];
    c->mem_w16((c->r[10] + (uint32_t)192), (uint16_t)c->r[3]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[7] = c->r[7] + (uint32_t)1;
    c->r[2] = c->r[2] & 15u;
    c->r[2] = c->r[2] << 8;
    c->mem_w16((c->r[8] + (uint32_t)2), (uint16_t)c->r[2]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[7] = c->r[7] + (uint32_t)1;
    c->r[2] = c->r[2] | c->r[3];
    c->mem_w16((c->r[8] + (uint32_t)2), (uint16_t)c->r[2]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[7] = c->r[7] + (uint32_t)1;
    c->r[3] = c->r[3] << 4;
    c->mem_w16((c->r[8] + (uint32_t)4), (uint16_t)c->r[3]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[2] = c->r[2] >> 4;
    c->r[3] = c->r[3] | c->r[2];
    c->mem_w16((c->r[8] + (uint32_t)4), (uint16_t)c->r[3]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[7] = c->r[7] + (uint32_t)1;
    c->r[3] = c->mem_r32((c->r[5] + (uint32_t)192));
    c->r[2] = c->r[2] & 15u;
    c->r[2] = c->r[2] << 8;
    c->mem_w16((c->r[3] + (uint32_t)56), (uint16_t)c->r[2]);
    c->r[4] = c->mem_r32((c->r[5] + (uint32_t)192));
    c->r[3] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)56));
    c->r[2] = c->r[2] | c->r[3];
    c->mem_w16((c->r[4] + (uint32_t)56), (uint16_t)c->r[2]);
    c->r[3] = c->mem_r32((c->r[5] + (uint32_t)192));
    c->r[2] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)56));
    c->r[7] = c->r[7] + (uint32_t)1;
    c->r[2] = c->r[2] << 3;
    c->mem_w16((c->r[3] + (uint32_t)56), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[7] = c->r[7] + (uint32_t)1;
    c->r[3] = c->mem_r32((c->r[5] + (uint32_t)192));
    c->r[2] = c->r[2] << 4;
    c->mem_w16((c->r[3] + (uint32_t)58), (uint16_t)c->r[2]);
    c->r[4] = c->mem_r32((c->r[5] + (uint32_t)192));
    c->r[2] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[3] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)58));
    c->r[2] = c->r[2] >> 4;
    c->r[3] = c->r[3] | c->r[2];
    c->mem_w16((c->r[4] + (uint32_t)58), (uint16_t)c->r[3]);
    c->r[3] = c->mem_r32((c->r[5] + (uint32_t)192));
    c->r[2] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)58));
    c->r[2] = c->r[2] << 3;
    c->mem_w16((c->r[3] + (uint32_t)58), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[7] = c->r[7] + (uint32_t)1;
    c->r[3] = c->mem_r32((c->r[5] + (uint32_t)192));
    c->r[2] = c->r[2] & 15u;
    c->r[2] = c->r[2] << 8;
    c->mem_w16((c->r[3] + (uint32_t)60), (uint16_t)c->r[2]);
    c->r[4] = c->mem_r32((c->r[5] + (uint32_t)192));
    c->r[3] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)60));
    c->r[2] = c->r[2] | c->r[3];
    c->mem_w16((c->r[4] + (uint32_t)60), (uint16_t)c->r[2]);
    c->r[3] = c->mem_r32((c->r[5] + (uint32_t)192));
    c->r[2] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)60));
    c->r[2] = c->r[2] << 3;
    c->mem_w16((c->r[3] + (uint32_t)60), (uint16_t)c->r[2]);
    c->r[2] = c->mem_r32((c->r[5] + (uint32_t)192));
    c->r[3] = (uint32_t)c->mem_r16((c->r[10] + (uint32_t)192));
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)8));
    c->r[4] = c->r[3] - c->r[2];
    c->mem_w16((c->r[10] + (uint32_t)192), (uint16_t)c->r[4]);
    c->r[2] = c->mem_r32((c->r[5] + (uint32_t)192));
    c->r[3] = (uint32_t)c->mem_r16((c->r[8] + (uint32_t)2));
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)10));
    c->r[6] = c->r[3] - c->r[2];
    c->mem_w16((c->r[8] + (uint32_t)2), (uint16_t)c->r[6]);
    c->r[2] = c->mem_r32((c->r[5] + (uint32_t)192));
    c->r[3] = (uint32_t)c->mem_r16((c->r[8] + (uint32_t)4));
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)12));
    c->r[7] = c->r[7] + (uint32_t)1;
    c->r[3] = c->r[3] - c->r[2];
    c->r[2] = c->r[4] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 2049);
    { int _t = (c->r[2] != c->r[0]); c->mem_w16((c->r[8] + (uint32_t)4), (uint16_t)c->r[3]); if (_t) goto L_80076378; }
    c->r[2] = c->r[4] + (uint32_t)-4096;
    c->mem_w16((c->r[10] + (uint32_t)192), (uint16_t)c->r[2]);
  L_80076378:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[10] + (uint32_t)192));
    c->r[4] = (uint32_t)c->mem_r16((c->r[10] + (uint32_t)192));
    c->r[2] = (uint32_t)((int32_t)c->r[2] < -2048);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[4] + (uint32_t)4096; if (_t) goto L_80076390; }
    c->mem_w16((c->r[10] + (uint32_t)192), (uint16_t)c->r[2]);
  L_80076390:;
    c->r[2] = c->r[6] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 2049);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[6] + (uint32_t)-4096; if (_t) goto L_800763A8; }
    c->mem_w16((c->r[8] + (uint32_t)2), (uint16_t)c->r[2]);
  L_800763A8:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[8] + (uint32_t)2));
    c->r[4] = (uint32_t)c->mem_r16((c->r[8] + (uint32_t)2));
    c->r[2] = (uint32_t)((int32_t)c->r[2] < -2048);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[4] + (uint32_t)4096; if (_t) goto L_800763C0; }
    c->mem_w16((c->r[8] + (uint32_t)2), (uint16_t)c->r[2]);
  L_800763C0:;
    c->r[2] = c->r[3] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 2049);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[3] + (uint32_t)-4096; if (_t) goto L_800763D8; }
    c->mem_w16((c->r[8] + (uint32_t)4), (uint16_t)c->r[2]);
  L_800763D8:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[8] + (uint32_t)4));
    c->r[3] = (uint32_t)c->mem_r16((c->r[8] + (uint32_t)4));
    c->r[2] = (uint32_t)((int32_t)c->r[2] < -2048);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[3] + (uint32_t)4096; if (_t) goto L_800763F0; }
    c->mem_w16((c->r[8] + (uint32_t)4), (uint16_t)c->r[2]);
  L_800763F0:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[10] + (uint32_t)192));
    cpu_div(c, c->r[2], c->r[9]);
    { int _t = (c->r[9] != c->r[0]);  if (_t) goto L_80076408; }
    rec_break(c, 7168u);
  L_80076408:;
    c->r[1] = c->r[0] + (uint32_t)-1;
    { int _t = (c->r[9] != c->r[1]); c->r[1] = (uint32_t)32768u << 16; if (_t) goto L_80076420; }
    { int _t = (c->r[2] != c->r[1]);  if (_t) goto L_80076420; }
    rec_break(c, 6144u);
  L_80076420:;
    c->r[2] = c->lo;
    c->r[3] = c->mem_r32((c->r[5] + (uint32_t)192));
    c->mem_w16((c->r[3] + (uint32_t)16), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[8] + (uint32_t)2));
    cpu_div(c, c->r[2], c->r[9]);
    { int _t = (c->r[9] != c->r[0]);  if (_t) goto L_80076448; }
    rec_break(c, 7168u);
  L_80076448:;
    c->r[1] = c->r[0] + (uint32_t)-1;
    { int _t = (c->r[9] != c->r[1]); c->r[1] = (uint32_t)32768u << 16; if (_t) goto L_80076460; }
    { int _t = (c->r[2] != c->r[1]);  if (_t) goto L_80076460; }
    rec_break(c, 6144u);
  L_80076460:;
    c->r[2] = c->lo;
    c->r[3] = c->mem_r32((c->r[5] + (uint32_t)192));
    c->mem_w16((c->r[3] + (uint32_t)18), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[8] + (uint32_t)4));
    cpu_div(c, c->r[2], c->r[9]);
    { int _t = (c->r[9] != c->r[0]);  if (_t) goto L_80076488; }
    rec_break(c, 7168u);
  L_80076488:;
    c->r[1] = c->r[0] + (uint32_t)-1;
    { int _t = (c->r[9] != c->r[1]); c->r[1] = (uint32_t)32768u << 16; if (_t) goto L_800764A0; }
    { int _t = (c->r[2] != c->r[1]);  if (_t) goto L_800764A0; }
    rec_break(c, 6144u);
  L_800764A0:;
    c->r[2] = c->lo;
    c->r[3] = c->mem_r32((c->r[5] + (uint32_t)192));
    c->r[11] = c->r[11] + (uint32_t)1;
    c->mem_w16((c->r[3] + (uint32_t)20), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[12] + (uint32_t)9));
    c->r[2] = (uint32_t)((int32_t)c->r[11] < (int32_t)c->r[2]);
    { int _t = (c->r[2] == c->r[0]); c->r[5] = c->r[5] + (uint32_t)4; if (_t) goto L_800768FC; }
     goto L_800761C4;
  L_800764CC:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[12] + (uint32_t)8));
    c->r[2] = c->r[2] & 128u;
    { int _t = (c->r[2] == c->r[0]); c->r[5] = (uint32_t)8064u << 16; if (_t) goto L_80076624; }
    c->r[4] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[7] = c->r[7] + (uint32_t)1;
    c->r[4] = c->r[4] << 24;
    c->r[4] = (uint32_t)((int32_t)c->r[4] >> 20);
    c->mem_w16((c->r[5] + (uint32_t)192), (uint16_t)c->r[4]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[2] = c->r[2] >> 4;
    c->r[4] = c->r[4] | c->r[2];
    c->mem_w16((c->r[5] + (uint32_t)192), (uint16_t)c->r[4]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[7] = c->r[7] + (uint32_t)1;
    c->r[5] = c->r[5] + (uint32_t)192;
    c->r[2] = c->r[2] << 28;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 20);
    c->mem_w16((c->r[5] + (uint32_t)2), (uint16_t)c->r[2]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[7] = c->r[7] + (uint32_t)1;
    c->r[2] = c->r[2] | c->r[3];
    c->mem_w16((c->r[5] + (uint32_t)2), (uint16_t)c->r[2]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[7] = c->r[7] + (uint32_t)1;
    c->r[3] = c->r[3] << 24;
    c->r[3] = (uint32_t)((int32_t)c->r[3] >> 20);
    c->mem_w16((c->r[5] + (uint32_t)4), (uint16_t)c->r[3]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[2] = c->r[2] >> 4;
    c->r[3] = c->r[3] | c->r[2];
    c->mem_w16((c->r[5] + (uint32_t)4), (uint16_t)c->r[3]);
    c->r[3] = c->r[8] << 16;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[12] + (uint32_t)136));
    c->r[3] = (uint32_t)((int32_t)c->r[3] >> 16);
    c->r[4] = c->r[4] - c->r[2];
    cpu_div(c, c->r[4], c->r[3]);
    { int _t = (c->r[3] != c->r[0]);  if (_t) goto L_80076578; }
    rec_break(c, 7168u);
  L_80076578:;
    c->r[1] = c->r[0] + (uint32_t)-1;
    { int _t = (c->r[3] != c->r[1]); c->r[1] = (uint32_t)32768u << 16; if (_t) goto L_80076590; }
    { int _t = (c->r[4] != c->r[1]);  if (_t) goto L_80076590; }
    rec_break(c, 6144u);
  L_80076590:;
    c->r[4] = c->lo;
    c->mem_w16((c->r[12] + (uint32_t)144), (uint16_t)c->r[4]);
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[5] + (uint32_t)2));
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[12] + (uint32_t)138));
    c->r[2] = c->r[2] - c->r[4];
    cpu_div(c, c->r[2], c->r[3]);
    { int _t = (c->r[3] != c->r[0]);  if (_t) goto L_800765BC; }
    rec_break(c, 7168u);
  L_800765BC:;
    c->r[1] = c->r[0] + (uint32_t)-1;
    { int _t = (c->r[3] != c->r[1]); c->r[1] = (uint32_t)32768u << 16; if (_t) goto L_800765D4; }
    { int _t = (c->r[2] != c->r[1]);  if (_t) goto L_800765D4; }
    rec_break(c, 6144u);
  L_800765D4:;
    c->r[2] = c->lo;
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[12] + (uint32_t)140));
    c->mem_w16((c->r[12] + (uint32_t)146), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[5] + (uint32_t)4));
    c->r[2] = c->r[2] - c->r[4];
    cpu_div(c, c->r[2], c->r[3]);
    { int _t = (c->r[3] != c->r[0]);  if (_t) goto L_800765FC; }
    rec_break(c, 7168u);
  L_800765FC:;
    c->r[1] = c->r[0] + (uint32_t)-1;
    { int _t = (c->r[3] != c->r[1]); c->r[1] = (uint32_t)32768u << 16; if (_t) goto L_80076614; }
    { int _t = (c->r[2] != c->r[1]);  if (_t) goto L_80076614; }
    rec_break(c, 6144u);
  L_80076614:;
    c->r[2] = c->lo;
    c->r[13] = c->r[0] + (uint32_t)1;
    c->mem_w16((c->r[12] + (uint32_t)148), (uint16_t)c->r[2]); goto L_80076628;
  L_80076624:;
    c->r[13] = c->r[0] + c->r[0];
  L_80076628:;
    c->r[11] = c->r[0] + c->r[0];
    c->r[2] = (uint32_t)c->mem_r8((c->r[12] + (uint32_t)8));
    c->r[3] = (uint32_t)c->mem_r8((c->r[12] + (uint32_t)9));
    c->r[2] = c->r[2] & 63u;
    { int _t = (c->r[3] == c->r[0]); c->mem_w8((c->r[12] + (uint32_t)8), (uint8_t)c->r[2]); if (_t) goto L_800768FC; }
    c->r[9] = (uint32_t)8064u << 16;
    c->r[6] = c->r[9] + (uint32_t)192;
    c->r[2] = c->r[8] << 16;
    c->r[10] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[8] = c->r[12] + c->r[0];
  L_80076654:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[12] + (uint32_t)8));
    c->r[2] = (uint32_t)((int32_t)c->r[11] < (int32_t)c->r[2]);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[11] + c->r[13]; if (_t) goto L_800768FC; }
    c->r[2] = c->r[2] & 1u;
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_800766E4; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[7] = c->r[7] + (uint32_t)1;
    c->r[2] = c->r[2] & 15u;
    c->r[2] = c->r[2] << 8;
    c->mem_w16((c->r[9] + (uint32_t)192), (uint16_t)c->r[2]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[7] = c->r[7] + (uint32_t)1;
    c->r[2] = c->r[2] | c->r[3];
    c->mem_w16((c->r[9] + (uint32_t)192), (uint16_t)c->r[2]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[7] = c->r[7] + (uint32_t)1;
    c->r[3] = c->r[3] << 4;
    c->mem_w16((c->r[6] + (uint32_t)2), (uint16_t)c->r[3]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[2] = c->r[2] >> 4;
    c->r[3] = c->r[3] | c->r[2];
    c->mem_w16((c->r[6] + (uint32_t)2), (uint16_t)c->r[3]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[7] = c->r[7] + (uint32_t)1;
    c->r[2] = c->r[2] & 15u;
    c->r[2] = c->r[2] << 8;
    c->mem_w16((c->r[6] + (uint32_t)4), (uint16_t)c->r[2]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[7] = c->r[7] + (uint32_t)1;
    c->r[2] = c->r[2] | c->r[3];
    c->mem_w16((c->r[6] + (uint32_t)4), (uint16_t)c->r[2]); goto L_80076750;
  L_800766E4:;
    c->r[3] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[7] = c->r[7] + (uint32_t)1;
    c->r[3] = c->r[3] << 4;
    c->mem_w16((c->r[9] + (uint32_t)192), (uint16_t)c->r[3]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[2] = c->r[2] >> 4;
    c->r[3] = c->r[3] | c->r[2];
    c->mem_w16((c->r[9] + (uint32_t)192), (uint16_t)c->r[3]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[7] = c->r[7] + (uint32_t)1;
    c->r[2] = c->r[2] & 15u;
    c->r[2] = c->r[2] << 8;
    c->mem_w16((c->r[6] + (uint32_t)2), (uint16_t)c->r[2]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[7] = c->r[7] + (uint32_t)1;
    c->r[2] = c->r[2] | c->r[3];
    c->mem_w16((c->r[6] + (uint32_t)2), (uint16_t)c->r[2]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[7] = c->r[7] + (uint32_t)1;
    c->r[3] = c->r[3] << 4;
    c->mem_w16((c->r[6] + (uint32_t)4), (uint16_t)c->r[3]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[7] + (uint32_t)0));
    c->r[2] = c->r[2] >> 4;
    c->r[3] = c->r[3] | c->r[2];
    c->mem_w16((c->r[6] + (uint32_t)4), (uint16_t)c->r[3]);
  L_80076750:;
    c->r[2] = c->mem_r32((c->r[8] + (uint32_t)192));
    c->r[3] = (uint32_t)c->mem_r16((c->r[9] + (uint32_t)192));
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)8));
    c->r[4] = c->r[3] - c->r[2];
    c->mem_w16((c->r[9] + (uint32_t)192), (uint16_t)c->r[4]);
    c->r[2] = c->mem_r32((c->r[8] + (uint32_t)192));
    c->r[3] = (uint32_t)c->mem_r16((c->r[6] + (uint32_t)2));
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)10));
    c->r[5] = c->r[3] - c->r[2];
    c->mem_w16((c->r[6] + (uint32_t)2), (uint16_t)c->r[5]);
    c->r[2] = c->mem_r32((c->r[8] + (uint32_t)192));
    c->r[3] = (uint32_t)c->mem_r16((c->r[6] + (uint32_t)4));
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)12));
    c->r[3] = c->r[3] - c->r[2];
    c->r[2] = c->r[4] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 2049);
    { int _t = (c->r[2] != c->r[0]); c->mem_w16((c->r[6] + (uint32_t)4), (uint16_t)c->r[3]); if (_t) goto L_800767B0; }
    c->r[2] = c->r[4] + (uint32_t)-4096;
    c->mem_w16((c->r[9] + (uint32_t)192), (uint16_t)c->r[2]);
  L_800767B0:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[9] + (uint32_t)192));
    c->r[4] = (uint32_t)c->mem_r16((c->r[9] + (uint32_t)192));
    c->r[2] = (uint32_t)((int32_t)c->r[2] < -2048);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[4] + (uint32_t)4096; if (_t) goto L_800767C8; }
    c->mem_w16((c->r[9] + (uint32_t)192), (uint16_t)c->r[2]);
  L_800767C8:;
    c->r[2] = c->r[5] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 2049);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[5] + (uint32_t)-4096; if (_t) goto L_800767E0; }
    c->mem_w16((c->r[6] + (uint32_t)2), (uint16_t)c->r[2]);
  L_800767E0:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[6] + (uint32_t)2));
    c->r[4] = (uint32_t)c->mem_r16((c->r[6] + (uint32_t)2));
    c->r[2] = (uint32_t)((int32_t)c->r[2] < -2048);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[4] + (uint32_t)4096; if (_t) goto L_800767F8; }
    c->mem_w16((c->r[6] + (uint32_t)2), (uint16_t)c->r[2]);
  L_800767F8:;
    c->r[2] = c->r[3] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 2049);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[3] + (uint32_t)-4096; if (_t) goto L_80076810; }
    c->mem_w16((c->r[6] + (uint32_t)4), (uint16_t)c->r[2]);
  L_80076810:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[6] + (uint32_t)4));
    c->r[3] = (uint32_t)c->mem_r16((c->r[6] + (uint32_t)4));
    c->r[2] = (uint32_t)((int32_t)c->r[2] < -2048);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[3] + (uint32_t)4096; if (_t) goto L_80076828; }
    c->mem_w16((c->r[6] + (uint32_t)4), (uint16_t)c->r[2]);
  L_80076828:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[9] + (uint32_t)192));
    cpu_div(c, c->r[2], c->r[10]);
    { int _t = (c->r[10] != c->r[0]);  if (_t) goto L_80076840; }
    rec_break(c, 7168u);
  L_80076840:;
    c->r[1] = c->r[0] + (uint32_t)-1;
    { int _t = (c->r[10] != c->r[1]); c->r[1] = (uint32_t)32768u << 16; if (_t) goto L_80076858; }
    { int _t = (c->r[2] != c->r[1]);  if (_t) goto L_80076858; }
    rec_break(c, 6144u);
  L_80076858:;
    c->r[2] = c->lo;
    c->r[3] = c->mem_r32((c->r[8] + (uint32_t)192));
    c->mem_w16((c->r[3] + (uint32_t)16), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[6] + (uint32_t)2));
    cpu_div(c, c->r[2], c->r[10]);
    { int _t = (c->r[10] != c->r[0]);  if (_t) goto L_80076880; }
    rec_break(c, 7168u);
  L_80076880:;
    c->r[1] = c->r[0] + (uint32_t)-1;
    { int _t = (c->r[10] != c->r[1]); c->r[1] = (uint32_t)32768u << 16; if (_t) goto L_80076898; }
    { int _t = (c->r[2] != c->r[1]);  if (_t) goto L_80076898; }
    rec_break(c, 6144u);
  L_80076898:;
    c->r[2] = c->lo;
    c->r[3] = c->mem_r32((c->r[8] + (uint32_t)192));
    c->mem_w16((c->r[3] + (uint32_t)18), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[6] + (uint32_t)4));
    cpu_div(c, c->r[2], c->r[10]);
    { int _t = (c->r[10] != c->r[0]);  if (_t) goto L_800768C0; }
    rec_break(c, 7168u);
  L_800768C0:;
    c->r[1] = c->r[0] + (uint32_t)-1;
    { int _t = (c->r[10] != c->r[1]); c->r[1] = (uint32_t)32768u << 16; if (_t) goto L_800768D8; }
    { int _t = (c->r[2] != c->r[1]);  if (_t) goto L_800768D8; }
    rec_break(c, 6144u);
  L_800768D8:;
    c->r[2] = c->lo;
    c->r[3] = c->mem_r32((c->r[8] + (uint32_t)192));
    c->r[11] = c->r[11] + (uint32_t)1;
    c->mem_w16((c->r[3] + (uint32_t)20), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[12] + (uint32_t)9));
    c->r[2] = (uint32_t)((int32_t)c->r[11] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]); c->r[8] = c->r[8] + (uint32_t)4; if (_t) goto L_80076654; }
  L_800768FC:;
     return;
    return;
}

static void leaf_800776F8(Core* c) {
    c->r[4] = c->r[4] - c->r[5];
    c->r[4] = c->r[4] & 4095u;
    c->r[7] = c->r[4] + c->r[0];
    { int _t = (c->r[7] != c->r[0]); c->r[2] = c->r[4] + c->r[0]; if (_t) goto L_80077714; }
    c->r[2] = c->r[5] & 4095u; return;
  L_80077714:;
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)2048);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[6] << 16; if (_t) goto L_80077738; }
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[7] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_8007775C; }
    c->r[2] = c->r[5] + c->r[6]; goto L_80077760;
  L_80077738:;
    c->r[3] = c->r[6] << 16;
    c->r[3] = (uint32_t)((int32_t)c->r[3] >> 16);
    c->r[2] = c->r[0] + (uint32_t)4096;
    c->r[2] = c->r[2] - c->r[3];
    c->r[2] = (uint32_t)((int32_t)c->r[2] < (int32_t)c->r[7]);
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_8007775C; }
    c->r[2] = c->r[5] - c->r[6]; goto L_80077760;
  L_8007775C:;
    c->r[2] = c->r[5] + c->r[4];
  L_80077760:;
    c->r[2] = c->r[2] & 4095u; return;
    return;
}

static void leaf_800782B0(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->r[5] = c->r[5] << 16;
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16);
    c->r[6] = c->r[6] << 16;
    c->r[6] = (uint32_t)((int32_t)c->r[6] >> 16);
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[31]);
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)10));
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)2));
    c->r[4] = c->r[2] - c->r[6];
    c->r[31] = 0x800782DCu;
    c->r[5] = c->r[5] - c->r[3]; func_80085690(c);
    c->r[2] = c->r[2] << 16;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_80078798(Core* c) {
    c->r[3] = (uint32_t)32778u << 16;
    c->r[3] = c->r[3] + (uint32_t)21672;
    c->r[2] = c->r[4] & 255u;
    c->r[2] = c->r[2] << 2;
    c->r[2] = c->r[2] + c->r[3];
    c->r[5] = c->r[5] & 255u;
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)0));
    c->r[5] = c->r[5] << 3;
    c->r[2] = c->r[2] + c->r[5];
    c->r[3] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)6));
    c->r[2] = (uint32_t)32778u << 16;
    c->r[2] = c->r[2] + (uint32_t)21936;
    c->r[4] = c->r[4] + c->r[2];
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)0));
    c->r[3] = c->r[3] & 1536u;
    c->r[3] = c->r[3] >> 9;
    c->r[2] = c->r[2] + c->r[3];
    c->r[3] = (uint32_t)32778u << 16;
    c->r[3] = c->r[3] + (uint32_t)11140;
    c->r[2] = c->r[2] << 2;
    c->r[2] = c->r[2] + c->r[3];
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)0));
     return;
    return;
}

static void leaf_80078824(Core* c) {
    c->r[2] = (uint32_t)32778u << 16;
    c->r[2] = c->r[2] + (uint32_t)21672;
    c->r[4] = (uint32_t)32780u << 16;
    c->r[3] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)-1936));
    c->r[4] = c->r[4] + (uint32_t)-1936;
    c->r[3] = c->r[3] << 2;
    c->r[3] = c->r[3] + c->r[2];
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)1));
    c->r[3] = c->mem_r32((c->r[3] + (uint32_t)0));
    c->r[2] = c->r[2] << 3;
    c->r[3] = c->r[3] + c->r[2];
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[3] + (uint32_t)0));
    c->r[2] = c->r[2] << 16;
    c->mem_w32((c->r[4] + (uint32_t)32), c->r[2]);
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[3] + (uint32_t)2));
    c->r[2] = c->r[2] << 16;
    c->mem_w32((c->r[4] + (uint32_t)36), c->r[2]);
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[3] + (uint32_t)4));
    c->r[2] = c->r[2] << 16;
    c->mem_w32((c->r[4] + (uint32_t)40), c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)6));
    c->r[2] = c->r[2] & 127u;
    c->mem_w8((c->r[4] + (uint32_t)1480), (uint8_t)c->r[2]); return;
    return;
}

static void leaf_800793C4(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-48;
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[20]);
    c->r[20] = c->r[5] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[19]);
    c->r[19] = c->r[0] + (uint32_t)160;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[0] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)40), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)36), c->r[21]);
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
    { int _t = ((int32_t)c->r[20] <= 0); c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]); if (_t) goto L_8007943C; }
    c->r[21] = c->r[19] + c->r[0];
    c->r[18] = c->r[6] + c->r[0];
    c->r[17] = c->r[4] + c->r[0];
  L_80079400:;
    c->r[4] = c->mem_r32((c->r[17] + (uint32_t)0));
    c->r[31] = 0x8007940Cu;
     func_8009A600(c);
    c->r[2] = c->r[2] >> 1;
    c->r[2] = c->r[2] << 3;
    c->r[3] = c->r[21] - c->r[2];
    c->r[2] = (uint32_t)((int32_t)c->r[3] < (int32_t)c->r[19]);
    { int _t = (c->r[2] == c->r[0]); c->mem_w32((c->r[18] + (uint32_t)0), c->r[3]); if (_t) goto L_80079428; }
    c->r[19] = c->r[3] + c->r[0];
  L_80079428:;
    c->r[18] = c->r[18] + (uint32_t)4;
    c->r[16] = c->r[16] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[16] < (int32_t)c->r[20]);
    { int _t = (c->r[2] != c->r[0]); c->r[17] = c->r[17] + (uint32_t)4; if (_t) goto L_80079400; }
  L_8007943C:;
    c->r[2] = c->r[19] + (uint32_t)-16;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)40));
    c->r[21] = c->mem_r32((c->r[29] + (uint32_t)36));
    c->r[20] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)48; return;
    return;
}

static void leaf_80079464(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->r[2] = (uint32_t)8064u << 16;
    c->r[4] = c->r[0] + (uint32_t)69;
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[31]);
    c->r[31] = 0x80079484u;
    c->mem_w16((c->r[2] + (uint32_t)380), (uint16_t)c->r[0]); func_8009A480(c);
    c->r[4] = (uint32_t)32789u << 16;
    c->r[4] = c->r[4] | 28672u;
    c->r[3] = (uint32_t)32783u << 16;
    c->r[2] = c->r[3] + (uint32_t)-12460;
    c->mem_w16((c->r[2] + (uint32_t)2), (uint16_t)c->r[0]);
    c->mem_w16((c->r[3] + (uint32_t)-12460), (uint16_t)c->r[0]);
    c->r[3] = (uint32_t)32780u << 16;
    c->r[2] = c->r[0] + (uint32_t)1;
    c->mem_w16((c->r[3] + (uint32_t)-4724), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)32780u << 16;
    c->r[16] = c->r[16] << 10;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->r[16] + c->r[4];
    c->mem_w32((c->r[2] + (uint32_t)-4728), c->r[16]);
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    c->r[5] = (uint32_t)32780u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)-4724));
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->mem_w16((c->r[5] + (uint32_t)-4724), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] & 65535u;
    { int _t = (c->r[2] != c->r[0]); c->r[6] = (uint32_t)32780u << 16; if (_t) goto L_80079520; }
    c->r[4] = c->mem_r32((c->r[6] + (uint32_t)-4728));
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)2));
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80079504; }
    c->r[4] = c->r[4] + (uint32_t)4;
  L_80079504:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)0));
    c->r[3] = (uint32_t)32783u << 16;
    c->mem_w16((c->r[3] + (uint32_t)-12460), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)2));
    c->r[4] = c->r[4] + (uint32_t)4;
    c->mem_w32((c->r[6] + (uint32_t)-4728), c->r[4]);
    c->mem_w16((c->r[5] + (uint32_t)-4724), (uint16_t)c->r[2]);
  L_80079520:;
     return;
    return;
}

static void leaf_8007982C(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = (uint32_t)32780u << 16;
    c->r[16] = c->r[16] + (uint32_t)-1936;
    c->r[4] = c->r[16] + c->r[0];
    c->r[5] = c->r[0] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[31]);
    c->r[31] = 0x80079850u;
    c->r[6] = c->r[0] + (uint32_t)1524; func_8009A420(c);
    c->r[2] = c->r[0] + (uint32_t)8;
    c->r[5] = c->r[0] + (uint32_t)4;
    c->mem_w8((c->r[16] + (uint32_t)12), (uint8_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)255;
    c->r[3] = (uint32_t)8064u << 16;
    c->mem_w8((c->r[16] + (uint32_t)13), (uint8_t)c->r[5]);
    c->mem_w8((c->r[16] + (uint32_t)28), (uint8_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)29), (uint8_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)30), (uint8_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)31), (uint8_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)2), (uint8_t)c->r[2]);
    c->mem_w8((c->r[16] + (uint32_t)1520), (uint8_t)c->r[2]);
    c->mem_w8((c->r[3] + (uint32_t)511), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)8064u << 16;
    c->r[3] = c->r[0] + (uint32_t)1;
    c->r[4] = c->r[0] + (uint32_t)2;
    c->mem_w16((c->r[2] + (uint32_t)632), (uint16_t)c->r[0]);
    c->r[2] = c->r[0] + (uint32_t)64;
    c->mem_w8((c->r[16] + (uint32_t)15), (uint8_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)351;
    c->mem_w16((c->r[16] + (uint32_t)352), (uint16_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)86;
    c->mem_w8((c->r[16] + (uint32_t)580), (uint8_t)c->r[3]);
    c->mem_w8((c->r[16] + (uint32_t)590), (uint8_t)c->r[3]);
    c->mem_w8((c->r[16] + (uint32_t)846), (uint8_t)c->r[3]);
    c->mem_w8((c->r[16] + (uint32_t)69), (uint8_t)c->r[3]);
    c->r[3] = c->r[0] + (uint32_t)1;
    c->mem_w8((c->r[16] + (uint32_t)354), (uint8_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)2280;
    c->mem_w8((c->r[16] + (uint32_t)836), (uint8_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)50), (uint8_t)c->r[4]);
    c->mem_w8((c->r[16] + (uint32_t)350), (uint8_t)c->r[5]);
    c->mem_w16((c->r[16] + (uint32_t)56), (uint16_t)c->r[3]);
    c->mem_w16((c->r[16] + (uint32_t)390), (uint16_t)c->r[2]);
    c->mem_w16((c->r[16] + (uint32_t)326), (uint16_t)c->r[0]);
    c->mem_w16((c->r[16] + (uint32_t)328), (uint16_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)433), (uint8_t)c->r[3]);
    c->mem_w8((c->r[16] + (uint32_t)44), (uint8_t)c->r[4]);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_8007A810(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-40;
    c->r[4] = (uint32_t)32782u << 16;
    c->r[4] = c->r[4] + (uint32_t)32384;
    c->r[5] = c->r[0] + c->r[0];
    c->r[6] = c->r[0] + (uint32_t)388;
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[19]);
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[31] = 0x8007A83Cu;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]); func_8009A420(c);
    c->r[17] = c->r[0] + c->r[0];
    c->r[19] = c->r[0] + (uint32_t)5;
    c->r[2] = (uint32_t)32784u << 16;
    c->r[2] = c->r[2] + (uint32_t)1680;
    c->r[18] = c->r[2] + (uint32_t)264;
    c->r[16] = c->r[2] + c->r[0];
    c->r[2] = (uint32_t)32783u << 16;
    c->mem_w32((c->r[2] + (uint32_t)10040), c->r[0]);
    c->r[2] = (uint32_t)32783u << 16;
    c->mem_w32((c->r[2] + (uint32_t)9120), c->r[0]);
  L_8007A864:;
    c->r[4] = c->r[16] + c->r[0];
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x8007A874u;
    c->r[6] = c->r[0] + (uint32_t)264; func_8009A420(c);
    c->mem_w32((c->r[16] + (uint32_t)36), c->r[18]);
    c->r[18] = c->r[18] + (uint32_t)264;
    c->mem_w8((c->r[16] + (uint32_t)40), (uint8_t)c->r[19]);
    c->r[17] = c->r[17] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[17] < 4);
    { int _t = (c->r[2] != c->r[0]); c->r[16] = c->r[16] + (uint32_t)264; if (_t) goto L_8007A864; }
    c->r[4] = (uint32_t)32784u << 16;
    c->r[4] = c->r[4] + (uint32_t)1680;
    c->r[3] = c->r[17] + (uint32_t)-1;
    c->r[2] = c->r[3] << 5;
    c->r[2] = c->r[2] + c->r[3];
    c->r[2] = c->r[2] << 3;
    c->r[2] = c->r[2] + c->r[4];
    c->mem_w32((c->r[2] + (uint32_t)36), c->r[0]);
    c->r[2] = (uint32_t)32783u << 16;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[3] = (uint32_t)32783u << 16;
    c->mem_w32((c->r[2] + (uint32_t)10044), c->r[4]);
    c->r[2] = c->r[0] + (uint32_t)4;
    c->mem_w8((c->r[3] + (uint32_t)9232), (uint8_t)c->r[2]);
    c->r[29] = c->r[29] + (uint32_t)40; return;
    return;
}

static void leaf_8007A8E0(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[31]);
    c->r[31] = 0x8007A8F0u;
     func_8007982C(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[2] + (uint32_t)380), (uint16_t)c->r[0]);
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_8007B0F0(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[0] + c->r[0];
    c->r[2] = (uint32_t)32783u << 16;
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[17] = c->r[2] + (uint32_t)10048;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[31]);
  L_8007B10C:;
    c->r[4] = c->r[17] + c->r[0];
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x8007B11Cu;
    c->r[6] = c->r[0] + (uint32_t)68; func_8009A420(c);
    c->r[16] = c->r[16] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[16] < 520);
    { int _t = (c->r[2] != c->r[0]); c->r[17] = c->r[17] + (uint32_t)68; if (_t) goto L_8007B10C; }
    c->r[2] = (uint32_t)32784u << 16;
    c->r[4] = c->r[2] + (uint32_t)-20196;
    c->r[3] = (uint32_t)32782u << 16;
    c->r[2] = (uint32_t)32783u << 16;
    c->r[2] = c->r[2] + (uint32_t)-10048;
    c->mem_w32((c->r[3] + (uint32_t)32372), c->r[2]);
    c->r[16] = c->r[0] + c->r[0];
    c->r[5] = c->r[3] + c->r[0];
  L_8007B14C:;
    c->r[2] = c->mem_r32((c->r[5] + (uint32_t)32372));
    c->r[16] = c->r[16] + (uint32_t)1;
    c->r[3] = c->r[2] + (uint32_t)-4;
    c->mem_w32((c->r[5] + (uint32_t)32372), c->r[3]);
    c->mem_w32((c->r[2] + (uint32_t)-4), c->r[4]);
    c->r[2] = (uint32_t)((int32_t)c->r[16] < 520);
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[4] + (uint32_t)-68; if (_t) goto L_8007B14C; }
    c->r[3] = (uint32_t)32783u << 16;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[2] = c->r[0] + (uint32_t)520;
    c->mem_w16((c->r[3] + (uint32_t)-12136), (uint16_t)c->r[2]);
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

static void leaf_8007B38C(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->r[3] = (uint32_t)32780u << 16;
    c->r[2] = (uint32_t)32784u << 16;
    c->r[2] = c->r[2] + (uint32_t)-20128;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[31]);
    c->r[4] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)6));
    c->r[3] = c->r[3] + (uint32_t)-1936;
    c->mem_w8((c->r[3] + (uint32_t)1500), (uint8_t)c->r[4]);
    c->r[5] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)1));
    c->r[6] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)2));
    c->r[7] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)3));
    c->r[8] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)4));
    c->r[9] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)5));
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)7));
    c->r[4] = c->r[4] & 255u;
    c->mem_w8((c->r[3] + (uint32_t)51), (uint8_t)c->r[5]);
    c->mem_w8((c->r[3] + (uint32_t)1502), (uint8_t)c->r[6]);
    c->mem_w8((c->r[3] + (uint32_t)1503), (uint8_t)c->r[7]);
    c->mem_w8((c->r[3] + (uint32_t)26), (uint8_t)c->r[8]);
    c->mem_w8((c->r[3] + (uint32_t)27), (uint8_t)c->r[9]);
    c->r[31] = 0x8007B3E4u;
    c->mem_w8((c->r[3] + (uint32_t)1501), (uint8_t)c->r[2]); func_8007B2C0(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_8007B45C(Core* c) {
    c->r[2] = (uint32_t)32782u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)32360));
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[31]);
    c->r[2] = c->r[2] & 4096u;
    { int _t = (c->r[2] == c->r[0]); c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]); if (_t) goto L_8007B4A4; }
    c->r[4] = c->r[0] + (uint32_t)17;
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x8007B488u;
    c->r[6] = c->r[5] + c->r[0]; func_80074590(c);
    c->r[2] = (uint32_t)8064u << 16;
    c->r[3] = c->mem_r32((c->r[2] + (uint32_t)312));
    c->r[2] = c->r[0] + (uint32_t)2;
    c->r[31] = 0x8007B49Cu;
    c->mem_w8((c->r[3] + (uint32_t)107), (uint8_t)c->r[2]); func_8007B38C(c);
     goto L_8007BE08;
  L_8007B4A4:;
    c->r[31] = 0x8007B4ACu;
     func_8007F078(c);
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)312));
    c->r[3] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)80));
    c->r[2] = (uint32_t)(c->r[3] < (uint32_t)5);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)32769u << 16; if (_t) goto L_8007BE08; }
    c->r[2] = c->r[2] + (uint32_t)28280;
    c->r[3] = c->r[3] << 2;
    c->r[3] = c->r[3] + c->r[2];
    c->r[2] = c->mem_r32((c->r[3] + (uint32_t)0));
    {  switch (c->r[2]) { case 0x8007B4E8u: goto L_8007B4E8; case 0x8007B5C8u: goto L_8007B5C8; case 0x8007B6D0u: goto L_8007B6D0; case 0x8007B864u: goto L_8007B864; case 0x8007BC3Cu: goto L_8007BC3C; default: rec_dispatch(c, c->r[2]); return; } }
  L_8007B4E8:;
    c->r[31] = 0x8007B4F0u;
     func_8007F104(c);
    c->r[2] = (uint32_t)32782u << 16;
    c->r[3] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)32360));
    c->r[2] = c->r[3] & 8192u;
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[0] + (uint32_t)20; if (_t) goto L_8007B540; }
    c->r[5] = c->r[0] + (uint32_t)-9;
    c->r[31] = 0x8007B514u;
    c->r[6] = c->r[0] + c->r[0]; func_80074590(c);
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)312));
    c->r[3] = c->r[0] + (uint32_t)1;
    c->mem_w8((c->r[2] + (uint32_t)107), (uint8_t)c->r[3]);
    c->r[2] = (uint32_t)32780u << 16;
    c->mem_w8((c->r[2] + (uint32_t)-2040), (uint8_t)c->r[0]);
    c->r[2] = (uint32_t)8064u << 16;
    c->r[31] = 0x8007B538u;
    c->mem_w8((c->r[2] + (uint32_t)310), (uint8_t)c->r[3]); func_8007B38C(c);
     goto L_8007BE08;
  L_8007B540:;
    c->r[2] = c->r[3] & 16384u;
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[0] + (uint32_t)17; if (_t) goto L_8007B57C; }
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x8007B558u;
    c->r[6] = c->r[5] + c->r[0]; func_80074590(c);
    c->r[3] = (uint32_t)8064u << 16;
    c->r[4] = (uint32_t)32780u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)-2040));
    c->r[3] = c->mem_r32((c->r[3] + (uint32_t)312));
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w16((c->r[3] + (uint32_t)80), (uint16_t)c->r[2]);
    c->mem_w16((c->r[3] + (uint32_t)96), (uint16_t)c->r[0]);
    c->mem_w8((c->r[4] + (uint32_t)-2040), (uint8_t)c->r[0]); goto L_8007BE08;
  L_8007B57C:;
    c->r[2] = c->r[3] & 16u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[3] & 64u; if (_t) goto L_8007B5A4; }
    c->r[3] = (uint32_t)32780u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)-2040));
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[2] + (uint32_t)-1; if (_t) goto L_8007BE08; }
    c->mem_w8((c->r[3] + (uint32_t)-2040), (uint8_t)c->r[2]); goto L_8007B84C;
  L_8007B5A4:;
    { int _t = (c->r[2] == c->r[0]); c->r[4] = (uint32_t)32780u << 16; if (_t) goto L_8007BE08; }
    c->r[3] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)-2040));
    c->r[2] = (uint32_t)(c->r[3] < (uint32_t)3);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[3] + (uint32_t)1; if (_t) goto L_8007BE08; }
    c->mem_w8((c->r[4] + (uint32_t)-2040), (uint8_t)c->r[2]); goto L_8007B84C;
  L_8007B5C8:;
    c->r[31] = 0x8007B5D0u;
     func_8007F250(c);
    c->r[2] = (uint32_t)32782u << 16;
    c->r[3] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)32360));
    c->r[2] = c->r[3] & 8192u;
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[0] + (uint32_t)20; if (_t) goto L_8007B610; }
    c->r[5] = c->r[0] + (uint32_t)-9;
    c->r[31] = 0x8007B5F4u;
    c->r[6] = c->r[0] + c->r[0]; func_80074590(c);
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)312));
    c->mem_w16((c->r[2] + (uint32_t)80), (uint16_t)c->r[0]);
    c->r[2] = (uint32_t)32780u << 16;
    c->mem_w8((c->r[2] + (uint32_t)-2040), (uint8_t)c->r[0]); goto L_8007BE08;
  L_8007B610:;
    c->r[2] = c->r[3] & 16u;
    { int _t = (c->r[2] != c->r[0]); c->r[16] = (uint32_t)32780u << 16; if (_t) goto L_8007BC90; }
    c->r[2] = c->r[3] & 64u;
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[3] & 32u; if (_t) goto L_8007BCCC; }
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)32780u << 16; if (_t) goto L_8007B67C; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-2040));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)32784u << 16; if (_t) goto L_8007B660; }
    c->r[4] = c->r[2] + (uint32_t)-20128;
    c->r[3] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)1));
    c->r[2] = (uint32_t)(c->r[3] < (uint32_t)2);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[3] + (uint32_t)1; if (_t) goto L_8007BE08; }
    c->mem_w8((c->r[4] + (uint32_t)1), (uint8_t)c->r[2]); goto L_8007B84C;
  L_8007B660:;
    c->r[3] = c->r[2] + (uint32_t)-20128;
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)2));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[2] + (uint32_t)1; if (_t) goto L_8007BE08; }
    c->mem_w8((c->r[3] + (uint32_t)2), (uint8_t)c->r[2]); goto L_8007B84C;
  L_8007B67C:;
    c->r[2] = c->r[3] & 128u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)32780u << 16; if (_t) goto L_8007BE08; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-2040));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)32784u << 16; if (_t) goto L_8007B6B4; }
    c->r[3] = c->r[2] + (uint32_t)-20128;
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)1));
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[2] + (uint32_t)-1; if (_t) goto L_8007BE08; }
    c->mem_w8((c->r[3] + (uint32_t)1), (uint8_t)c->r[2]); goto L_8007B84C;
  L_8007B6B4:;
    c->r[3] = c->r[2] + (uint32_t)-20128;
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)2));
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[2] + (uint32_t)-1; if (_t) goto L_8007BE08; }
    c->mem_w8((c->r[3] + (uint32_t)2), (uint8_t)c->r[2]); goto L_8007B84C;
  L_8007B6D0:;
    c->r[31] = 0x8007B6D8u;
     func_8007F498(c);
    c->r[2] = (uint32_t)32782u << 16;
    c->r[3] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)32360));
    c->r[2] = c->r[3] & 8192u;
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[0] + (uint32_t)20; if (_t) goto L_8007B718; }
    c->r[5] = c->r[0] + (uint32_t)-9;
    c->r[31] = 0x8007B6FCu;
    c->r[6] = c->r[0] + c->r[0]; func_80074590(c);
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)312));
    c->r[3] = (uint32_t)32780u << 16;
    c->mem_w16((c->r[2] + (uint32_t)80), (uint16_t)c->r[0]);
    c->r[2] = c->r[0] + (uint32_t)1;
    c->mem_w8((c->r[3] + (uint32_t)-2040), (uint8_t)c->r[2]); goto L_8007BE08;
  L_8007B718:;
    c->r[2] = c->r[3] & 16u;
    { int _t = (c->r[2] != c->r[0]); c->r[16] = (uint32_t)32780u << 16; if (_t) goto L_8007BC90; }
    c->r[2] = c->r[3] & 64u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[3] & 32u; if (_t) goto L_8007B74C; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)-2040));
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)2);
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[0] + (uint32_t)21; if (_t) goto L_8007BE08; }
    c->r[5] = c->r[0] + c->r[0]; goto L_8007BCE0;
  L_8007B74C:;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)32780u << 16; if (_t) goto L_8007B7CC; }
    c->r[3] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-2040));
    { int _t = (c->r[3] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_8007B784; }
    c->r[2] = (uint32_t)32784u << 16;
    c->r[3] = c->r[2] + (uint32_t)-20128;
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)3));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[2] + (uint32_t)1; if (_t) goto L_8007BE08; }
    c->mem_w8((c->r[3] + (uint32_t)3), (uint8_t)c->r[2]); goto L_8007B844;
  L_8007B784:;
    { int _t = (c->r[3] != c->r[2]); c->r[2] = (uint32_t)32784u << 16; if (_t) goto L_8007B7AC; }
    c->r[4] = c->r[2] + (uint32_t)-20128;
    c->r[3] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)4));
    c->r[2] = (uint32_t)(c->r[3] < (uint32_t)9);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[3] + (uint32_t)1; if (_t) goto L_8007BE08; }
    c->mem_w8((c->r[4] + (uint32_t)4), (uint8_t)c->r[2]); goto L_8007B844;
  L_8007B7AC:;
    c->r[4] = c->r[2] + (uint32_t)-20128;
    c->r[3] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)5));
    c->r[2] = (uint32_t)(c->r[3] < (uint32_t)9);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[3] + (uint32_t)1; if (_t) goto L_8007BE08; }
    c->mem_w8((c->r[4] + (uint32_t)5), (uint8_t)c->r[2]); goto L_8007B844;
  L_8007B7CC:;
    c->r[2] = c->r[3] & 128u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)32780u << 16; if (_t) goto L_8007BE08; }
    c->r[3] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-2040));
    { int _t = (c->r[3] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_8007B808; }
    c->r[2] = (uint32_t)32784u << 16;
    c->r[3] = c->r[2] + (uint32_t)-20128;
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)3));
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[2] + (uint32_t)-1; if (_t) goto L_8007BE08; }
    c->mem_w8((c->r[3] + (uint32_t)3), (uint8_t)c->r[2]); goto L_8007B844;
  L_8007B808:;
    { int _t = (c->r[3] != c->r[2]); c->r[2] = (uint32_t)32784u << 16; if (_t) goto L_8007B82C; }
    c->r[3] = c->r[2] + (uint32_t)-20128;
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)4));
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[2] + (uint32_t)-1; if (_t) goto L_8007BE08; }
    c->mem_w8((c->r[3] + (uint32_t)4), (uint8_t)c->r[2]); goto L_8007B844;
  L_8007B82C:;
    c->r[3] = c->r[2] + (uint32_t)-20128;
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)5));
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[2] + (uint32_t)-1; if (_t) goto L_8007BE08; }
    c->mem_w8((c->r[3] + (uint32_t)5), (uint8_t)c->r[2]);
  L_8007B844:;
    c->r[31] = 0x8007B84Cu;
     func_80075D58(c);
  L_8007B84C:;
    c->r[4] = c->r[0] + (uint32_t)21;
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x8007B85Cu;
    c->r[6] = c->r[5] + c->r[0]; func_80074590(c);
     goto L_8007BE08;
  L_8007B864:;
    c->r[16] = (uint32_t)8064u << 16;
    c->r[2] = c->r[0] + (uint32_t)1;
    c->r[31] = 0x8007B874u;
    c->mem_w8((c->r[16] + (uint32_t)310), (uint8_t)c->r[2]); func_8007F73C(c);
    c->r[2] = (uint32_t)32782u << 16;
    c->r[4] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)32360));
    c->r[2] = c->r[4] & 8192u;
    { int _t = (c->r[2] == c->r[0]); c->r[5] = c->r[0] + (uint32_t)-9; if (_t) goto L_8007B8B8; }
    c->r[4] = c->r[0] + (uint32_t)20;
    c->r[31] = 0x8007B898u;
    c->r[6] = c->r[0] + c->r[0]; func_80074590(c);
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)312));
    c->r[3] = c->r[0] + (uint32_t)2;
    c->mem_w8((c->r[16] + (uint32_t)310), (uint8_t)c->r[3]);
    c->mem_w16((c->r[2] + (uint32_t)80), (uint16_t)c->r[0]);
    c->r[2] = (uint32_t)32780u << 16;
    c->mem_w8((c->r[2] + (uint32_t)-2040), (uint8_t)c->r[3]); goto L_8007BE08;
  L_8007B8B8:;
    c->r[2] = c->r[4] & 16u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_8007B91C; }
    c->r[16] = (uint32_t)32782u << 16;
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)312));
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)32370));
    { int _t = ((int32_t)c->r[3] <= 0); c->mem_w16((c->r[2] + (uint32_t)96), (uint16_t)c->r[0]); if (_t) goto L_8007BE08; }
    c->r[4] = c->r[0] + (uint32_t)21;
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x8007B8ECu;
    c->r[6] = c->r[5] + c->r[0]; func_80074590(c);
    c->r[3] = (uint32_t)32783u << 16;
    c->r[3] = c->r[3] + (uint32_t)-32600;
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)32370));
    c->r[4] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)16506));
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->mem_w16((c->r[16] + (uint32_t)32370), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)8202));
    c->r[4] = c->r[4] + (uint32_t)-1;
    c->mem_w16((c->r[3] + (uint32_t)16506), (uint16_t)c->r[4]);
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->mem_w16((c->r[3] + (uint32_t)8202), (uint16_t)c->r[2]); goto L_8007BE08;
  L_8007B91C:;
    c->r[2] = (uint32_t)32783u << 16;
    c->r[3] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)-12460));
    c->r[2] = c->r[3] & 16u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_8007B998; }
    c->r[3] = c->mem_r32((c->r[2] + (uint32_t)312));
    c->r[2] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)96));
    c->r[4] = c->r[2] + (uint32_t)1;
    c->r[2] = c->r[4] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 8);
    { int _t = (c->r[2] != c->r[0]); c->mem_w16((c->r[3] + (uint32_t)96), (uint16_t)c->r[4]); if (_t) goto L_8007BE08; }
    c->r[2] = c->r[4] & 1u;
    { int _t = (c->r[2] == c->r[0]); c->r[4] = (uint32_t)32782u << 16; if (_t) goto L_8007BE08; }
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)32370));
    c->r[3] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)32370));
    { int _t = ((int32_t)c->r[2] <= 0); c->r[2] = c->r[3] + (uint32_t)-1; if (_t) goto L_8007BE08; }
    c->r[3] = (uint32_t)32783u << 16;
    c->r[3] = c->r[3] + (uint32_t)-32600;
    c->mem_w16((c->r[4] + (uint32_t)32370), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)8202));
    c->r[4] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)16506));
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->r[4] = c->r[4] + (uint32_t)-1; goto L_8007BA70;
  L_8007B998:;
    c->r[2] = c->r[4] & 64u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_8007BA00; }
    c->r[16] = (uint32_t)32782u << 16;
    c->r[3] = c->mem_r32((c->r[2] + (uint32_t)312));
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)32370));
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 16);
    { int _t = (c->r[2] == c->r[0]); c->mem_w16((c->r[3] + (uint32_t)96), (uint16_t)c->r[0]); if (_t) goto L_8007BE08; }
    c->r[4] = c->r[0] + (uint32_t)21;
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x8007B9D0u;
    c->r[6] = c->r[5] + c->r[0]; func_80074590(c);
    c->r[3] = (uint32_t)32783u << 16;
    c->r[3] = c->r[3] + (uint32_t)-32600;
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)32370));
    c->r[4] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)16506));
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w16((c->r[16] + (uint32_t)32370), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)8202));
    c->r[4] = c->r[4] + (uint32_t)1;
    c->mem_w16((c->r[3] + (uint32_t)16506), (uint16_t)c->r[4]);
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w16((c->r[3] + (uint32_t)8202), (uint16_t)c->r[2]); goto L_8007BE08;
  L_8007BA00:;
    c->r[2] = c->r[3] & 64u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_8007BA7C; }
    c->r[3] = c->mem_r32((c->r[2] + (uint32_t)312));
    c->r[2] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)96));
    c->r[4] = c->r[2] + (uint32_t)1;
    c->r[2] = c->r[4] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 8);
    { int _t = (c->r[2] != c->r[0]); c->mem_w16((c->r[3] + (uint32_t)96), (uint16_t)c->r[4]); if (_t) goto L_8007BE08; }
    c->r[2] = c->r[4] & 1u;
    { int _t = (c->r[2] == c->r[0]); c->r[4] = (uint32_t)32782u << 16; if (_t) goto L_8007BE08; }
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)32370));
    c->r[3] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)32370));
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 16);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[3] + (uint32_t)1; if (_t) goto L_8007BE08; }
    c->r[3] = (uint32_t)32783u << 16;
    c->r[3] = c->r[3] + (uint32_t)-32600;
    c->mem_w16((c->r[4] + (uint32_t)32370), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)8202));
    c->r[4] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)16506));
    c->r[2] = c->r[2] + (uint32_t)1;
    c->r[4] = c->r[4] + (uint32_t)1;
  L_8007BA70:;
    c->mem_w16((c->r[3] + (uint32_t)8202), (uint16_t)c->r[2]);
    c->mem_w16((c->r[3] + (uint32_t)16506), (uint16_t)c->r[4]); goto L_8007BE08;
  L_8007BA7C:;
    c->r[2] = c->r[4] & 32u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_8007BAE4; }
    c->r[16] = (uint32_t)32782u << 16;
    c->r[3] = c->mem_r32((c->r[2] + (uint32_t)312));
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)32368));
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 8);
    { int _t = (c->r[2] == c->r[0]); c->mem_w16((c->r[3] + (uint32_t)96), (uint16_t)c->r[0]); if (_t) goto L_8007BE08; }
    c->r[4] = c->r[0] + (uint32_t)21;
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x8007BAB4u;
    c->r[6] = c->r[5] + c->r[0]; func_80074590(c);
    c->r[3] = (uint32_t)32783u << 16;
    c->r[3] = c->r[3] + (uint32_t)-32600;
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)32368));
    c->r[4] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)16504));
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w16((c->r[16] + (uint32_t)32368), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)8200));
    c->r[4] = c->r[4] + (uint32_t)1;
    c->mem_w16((c->r[3] + (uint32_t)16504), (uint16_t)c->r[4]);
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w16((c->r[3] + (uint32_t)8200), (uint16_t)c->r[2]); goto L_8007BE08;
  L_8007BAE4:;
    c->r[2] = c->r[3] & 32u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_8007BB58; }
    c->r[3] = c->mem_r32((c->r[2] + (uint32_t)312));
    c->r[2] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)96));
    c->r[4] = c->r[2] + (uint32_t)1;
    c->r[2] = c->r[4] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 8);
    { int _t = (c->r[2] != c->r[0]); c->mem_w16((c->r[3] + (uint32_t)96), (uint16_t)c->r[4]); if (_t) goto L_8007BE08; }
    c->r[2] = c->r[4] & 1u;
    { int _t = (c->r[2] == c->r[0]); c->r[4] = (uint32_t)32782u << 16; if (_t) goto L_8007BE08; }
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)32368));
    c->r[3] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)32368));
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 8);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[3] + (uint32_t)1; if (_t) goto L_8007BE08; }
    c->r[3] = (uint32_t)32783u << 16;
    c->r[3] = c->r[3] + (uint32_t)-32600;
    c->mem_w16((c->r[4] + (uint32_t)32368), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)8200));
    c->r[4] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)16504));
    c->r[2] = c->r[2] + (uint32_t)1;
    c->r[4] = c->r[4] + (uint32_t)1; goto L_8007BC30;
  L_8007BB58:;
    c->r[2] = c->r[4] & 128u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_8007BBC0; }
    c->r[16] = (uint32_t)32782u << 16;
    c->r[3] = c->mem_r32((c->r[2] + (uint32_t)312));
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)32368));
    c->r[2] = (uint32_t)((int32_t)c->r[2] < -7);
    { int _t = (c->r[2] != c->r[0]); c->mem_w16((c->r[3] + (uint32_t)96), (uint16_t)c->r[0]); if (_t) goto L_8007BE08; }
    c->r[4] = c->r[0] + (uint32_t)21;
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x8007BB90u;
    c->r[6] = c->r[5] + c->r[0]; func_80074590(c);
    c->r[3] = (uint32_t)32783u << 16;
    c->r[3] = c->r[3] + (uint32_t)-32600;
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)32368));
    c->r[4] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)16504));
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->mem_w16((c->r[16] + (uint32_t)32368), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)8200));
    c->r[4] = c->r[4] + (uint32_t)-1;
    c->mem_w16((c->r[3] + (uint32_t)16504), (uint16_t)c->r[4]);
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->mem_w16((c->r[3] + (uint32_t)8200), (uint16_t)c->r[2]); goto L_8007BE08;
  L_8007BBC0:;
    c->r[2] = c->r[3] & 128u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_8007BE08; }
    c->r[3] = c->mem_r32((c->r[2] + (uint32_t)312));
    c->r[2] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)96));
    c->r[4] = c->r[2] + (uint32_t)1;
    c->r[2] = c->r[4] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 8);
    { int _t = (c->r[2] != c->r[0]); c->mem_w16((c->r[3] + (uint32_t)96), (uint16_t)c->r[4]); if (_t) goto L_8007BE08; }
    c->r[2] = c->r[4] & 1u;
    { int _t = (c->r[2] == c->r[0]); c->r[4] = (uint32_t)32782u << 16; if (_t) goto L_8007BE08; }
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)32368));
    c->r[3] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)32368));
    c->r[2] = (uint32_t)((int32_t)c->r[2] < -7);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[3] + (uint32_t)-1; if (_t) goto L_8007BE08; }
    c->r[3] = (uint32_t)32783u << 16;
    c->r[3] = c->r[3] + (uint32_t)-32600;
    c->mem_w16((c->r[4] + (uint32_t)32368), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)8200));
    c->r[4] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)16504));
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->r[4] = c->r[4] + (uint32_t)-1;
  L_8007BC30:;
    c->mem_w16((c->r[3] + (uint32_t)8200), (uint16_t)c->r[2]);
    c->mem_w16((c->r[3] + (uint32_t)16504), (uint16_t)c->r[4]); goto L_8007BE08;
  L_8007BC3C:;
    c->r[31] = 0x8007BC44u;
     func_8007F8F8(c);
    c->r[2] = (uint32_t)32782u << 16;
    c->r[3] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)32360));
    c->r[2] = c->r[3] & 8192u;
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[0] + (uint32_t)20; if (_t) goto L_8007BC84; }
    c->r[5] = c->r[0] + (uint32_t)-9;
    c->r[31] = 0x8007BC68u;
    c->r[6] = c->r[0] + c->r[0]; func_80074590(c);
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)312));
    c->r[3] = (uint32_t)32780u << 16;
    c->mem_w16((c->r[2] + (uint32_t)80), (uint16_t)c->r[0]);
    c->r[2] = c->r[0] + (uint32_t)3;
    c->mem_w8((c->r[3] + (uint32_t)-2040), (uint8_t)c->r[2]); goto L_8007BE08;
  L_8007BC84:;
    c->r[2] = c->r[3] & 16u;
    { int _t = (c->r[2] == c->r[0]); c->r[16] = (uint32_t)32780u << 16; if (_t) goto L_8007BCC0; }
  L_8007BC90:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)-2040));
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[0] + (uint32_t)21; if (_t) goto L_8007BE08; }
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x8007BCACu;
    c->r[6] = c->r[5] + c->r[0]; func_80074590(c);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)-2040));
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->mem_w8((c->r[16] + (uint32_t)-2040), (uint8_t)c->r[2]); goto L_8007BE08;
  L_8007BCC0:;
    c->r[2] = c->r[3] & 64u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[3] & 32u; if (_t) goto L_8007BCFC; }
  L_8007BCCC:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)-2040));
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[0] + (uint32_t)21; if (_t) goto L_8007BE08; }
    c->r[5] = c->r[0] + c->r[0];
  L_8007BCE0:;
    c->r[31] = 0x8007BCE8u;
    c->r[6] = c->r[5] + c->r[0]; func_80074590(c);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)-2040));
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w8((c->r[16] + (uint32_t)-2040), (uint8_t)c->r[2]); goto L_8007BE08;
  L_8007BCFC:;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)32780u << 16; if (_t) goto L_8007BD74; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-2040));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)32784u << 16; if (_t) goto L_8007BD48; }
    c->r[16] = c->r[2] + (uint32_t)-20128;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)7));
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[0] + (uint32_t)21; if (_t) goto L_8007BE08; }
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x8007BD34u;
    c->r[6] = c->r[5] + c->r[0]; func_80074590(c);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)7));
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w8((c->r[16] + (uint32_t)7), (uint8_t)c->r[2]); goto L_8007BE08;
  L_8007BD48:;
    c->r[16] = c->r[2] + (uint32_t)-20128;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)6));
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[0] + (uint32_t)21; if (_t) goto L_8007BE08; }
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x8007BD68u;
    c->r[6] = c->r[5] + c->r[0]; func_80074590(c);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)6));
    c->r[2] = c->r[2] + (uint32_t)1; goto L_8007BE04;
  L_8007BD74:;
    c->r[2] = c->r[3] & 128u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)32780u << 16; if (_t) goto L_8007BE08; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-2040));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)32784u << 16; if (_t) goto L_8007BDD8; }
    c->r[16] = c->r[2] + (uint32_t)-20128;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)7));
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[0] + (uint32_t)21; if (_t) goto L_8007BE08; }
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x8007BDB0u;
    c->r[6] = c->r[5] + c->r[0]; func_80074590(c);
    c->r[4] = c->r[0] + c->r[0];
    c->r[5] = c->r[0] + (uint32_t)1;
    c->r[6] = c->r[0] + (uint32_t)255;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)7));
    c->r[7] = c->r[0] + (uint32_t)2;
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->r[31] = 0x8007BDD0u;
    c->mem_w8((c->r[16] + (uint32_t)7), (uint8_t)c->r[2]); func_800521F4(c);
     goto L_8007BE08;
  L_8007BDD8:;
    c->r[16] = c->r[2] + (uint32_t)-20128;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)6));
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[0] + (uint32_t)21; if (_t) goto L_8007BE08; }
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x8007BDF8u;
    c->r[6] = c->r[5] + c->r[0]; func_80074590(c);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)6));
    c->r[2] = c->r[2] + (uint32_t)-1;
  L_8007BE04:;
    c->mem_w8((c->r[16] + (uint32_t)6), (uint8_t)c->r[2]);
  L_8007BE08:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_8007BE18(Core* c) {
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)312));
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[31]);
    c->r[3] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)80));
    c->r[2] = (uint32_t)(c->r[3] < (uint32_t)6);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)32769u << 16; if (_t) goto L_8007BF10; }
    c->r[2] = c->r[2] + (uint32_t)28304;
    c->r[3] = c->r[3] << 2;
    c->r[3] = c->r[3] + c->r[2];
    c->r[2] = c->mem_r32((c->r[3] + (uint32_t)0));
    {  switch (c->r[2]) { case 0x8007BE58u: goto L_8007BE58; case 0x8007BE68u: goto L_8007BE68; case 0x8007BE78u: goto L_8007BE78; case 0x8007BEB4u: goto L_8007BEB4; case 0x8007BED4u: goto L_8007BED4; case 0x8007BEE4u: goto L_8007BEE4; default: rec_dispatch(c, c->r[2]); return; } }
  L_8007BE58:;
    c->r[31] = 0x8007BE60u;
    c->r[4] = c->r[4] & 255u; rec_dispatch(c, 0x8018FA88u);
     goto L_8007BF10;
  L_8007BE68:;
    c->r[31] = 0x8007BE70u;
     rec_dispatch(c, 0x8018FBCCu);
     goto L_8007BF10;
  L_8007BE78:;
    c->r[4] = c->r[0] + (uint32_t)20;
    c->r[5] = c->r[0] + (uint32_t)-9;
    c->r[31] = 0x8007BE88u;
    c->r[6] = c->r[0] + c->r[0]; func_80074590(c);
    c->r[4] = (uint32_t)8064u << 16;
    c->r[2] = c->mem_r32((c->r[4] + (uint32_t)312));
    c->r[3] = c->r[0] + (uint32_t)1;
    c->mem_w8((c->r[2] + (uint32_t)107), (uint8_t)c->r[3]);
    c->r[4] = c->mem_r32((c->r[4] + (uint32_t)312));
    c->r[2] = (uint32_t)32780u << 16;
    c->mem_w8((c->r[2] + (uint32_t)-2040), (uint8_t)c->r[3]);
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w8((c->r[2] + (uint32_t)310), (uint8_t)c->r[3]);
    c->mem_w16((c->r[4] + (uint32_t)80), (uint16_t)c->r[0]); goto L_8007BF10;
  L_8007BEB4:;
    c->r[4] = c->r[0] + (uint32_t)17;
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x8007BEC4u;
    c->r[6] = c->r[5] + c->r[0]; func_80074590(c);
    c->r[4] = (uint32_t)8064u << 16;
    c->r[3] = c->mem_r32((c->r[4] + (uint32_t)312));
    c->r[2] = c->r[0] + (uint32_t)2; goto L_8007BF00;
  L_8007BED4:;
    c->r[4] = (uint32_t)8064u << 16;
    c->r[3] = c->mem_r32((c->r[4] + (uint32_t)312));
    c->r[2] = c->r[0] + (uint32_t)7; goto L_8007BF00;
  L_8007BEE4:;
    c->r[4] = c->r[0] + (uint32_t)17;
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x8007BEF4u;
    c->r[6] = c->r[5] + c->r[0]; func_80074590(c);
    c->r[4] = (uint32_t)8064u << 16;
    c->r[3] = c->mem_r32((c->r[4] + (uint32_t)312));
    c->r[2] = c->r[0] + (uint32_t)9;
  L_8007BF00:;
    c->mem_w8((c->r[3] + (uint32_t)107), (uint8_t)c->r[2]);
    c->r[2] = c->mem_r32((c->r[4] + (uint32_t)312));
    c->mem_w16((c->r[2] + (uint32_t)80), (uint16_t)c->r[0]);
  L_8007BF10:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_8007BF20(Core* c) {
    c->r[2] = (uint32_t)32780u << 16;
    c->r[3] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-1974));
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[17] = c->r[5] + c->r[0];
    c->r[2] = (uint32_t)(c->r[3] < (uint32_t)5);
    { int _t = (c->r[2] == c->r[0]); c->mem_w32((c->r[29] + (uint32_t)24), c->r[31]); if (_t) goto L_8007C0BC; }
    c->r[2] = (uint32_t)32769u << 16;
    c->r[2] = c->r[2] + (uint32_t)28328;
    c->r[3] = c->r[3] << 2;
    c->r[3] = c->r[3] + c->r[2];
    c->r[2] = c->mem_r32((c->r[3] + (uint32_t)0));
    {  switch (c->r[2]) { case 0x8007BF68u: goto L_8007BF68; case 0x8007BFC4u: goto L_8007BFC4; case 0x8007C024u: goto L_8007C024; case 0x8007C034u: goto L_8007C034; case 0x8007C060u: goto L_8007C060; default: rec_dispatch(c, c->r[2]); return; } }
  L_8007BF68:;
    c->r[31] = 0x8007BF70u;
     func_8001CF2C(c);
    c->r[31] = 0x8007BF78u;
    c->r[4] = c->r[0] + (uint32_t)1; func_80045558(c);
    c->r[3] = (uint32_t)32780u << 16;
    c->r[3] = c->r[3] + (uint32_t)-2040;
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)66));
    c->r[2] = c->r[2] + (uint32_t)1;
    { int _t = (c->r[17] != c->r[0]); c->mem_w8((c->r[3] + (uint32_t)66), (uint8_t)c->r[2]); if (_t) goto L_8007BF9C; }
    c->r[4] = c->r[0] + (uint32_t)12; goto L_8007BFB4;
  L_8007BF9C:;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)311));
    c->r[2] = c->r[0] + (uint32_t)1;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_8007C0BC; }
    c->r[4] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)571));
  L_8007BFB4:;
    c->r[31] = 0x8007BFBCu;
    c->r[5] = c->r[0] + (uint32_t)1; func_800750D8(c);
     goto L_8007C0BC;
  L_8007BFC4:;
    c->r[31] = 0x8007BFCCu;
    c->r[4] = c->r[16] + c->r[0]; func_8007BE18(c);
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)312));
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)80));
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)2);
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_8007C0BC; }
    { int _t = (c->r[17] != c->r[0]); c->r[3] = (uint32_t)32780u << 16; if (_t) goto L_8007C004; }
    c->r[3] = (uint32_t)32780u << 16;
    c->r[2] = c->r[0] + (uint32_t)3;
    c->mem_w8((c->r[3] + (uint32_t)-1974), (uint8_t)c->r[2]); goto L_8007C0BC;
  L_8007C004:;
    c->r[3] = c->r[3] + (uint32_t)-2040;
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)66));
    c->r[2] = c->r[2] + (uint32_t)1;
    c->r[31] = 0x8007C01Cu;
    c->mem_w8((c->r[3] + (uint32_t)66), (uint8_t)c->r[2]); func_8001CF2C(c);
     goto L_8007C0BC;
  L_8007C024:;
    c->r[31] = 0x8007C02Cu;
    c->r[4] = c->r[0] + (uint32_t)1; func_80045580(c);
     goto L_8007C03C;
  L_8007C034:;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)411));
  L_8007C03C:;
    { int _t = (c->r[2] == c->r[0]); c->r[3] = (uint32_t)32780u << 16; if (_t) goto L_8007C0BC; }
    c->r[3] = c->r[3] + (uint32_t)-2040;
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)66));
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w8((c->r[3] + (uint32_t)66), (uint8_t)c->r[2]); goto L_8007C0BC;
  L_8007C060:;
    c->r[31] = 0x8007C068u;
    c->r[4] = c->r[16] + c->r[0]; func_8007BE18(c);
    { int _t = (c->r[16] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_8007C08C; }
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)312));
    c->r[3] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)107));
    c->r[2] = c->r[0] + (uint32_t)7;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_8007C0B4; }
  L_8007C08C:;
    { int _t = (c->r[17] != c->r[2]); c->r[2] = (uint32_t)32780u << 16; if (_t) goto L_8007C0B8; }
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)311));
    { int _t = (c->r[2] == c->r[17]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_8007C0B4; }
    c->r[4] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)571));
    c->r[31] = 0x8007C0B4u;
    c->r[5] = c->r[0] + (uint32_t)1; func_800750D8(c);
  L_8007C0B4:;
    c->r[2] = (uint32_t)32780u << 16;
  L_8007C0B8:;
    c->mem_w8((c->r[2] + (uint32_t)-1974), (uint8_t)c->r[0]);
  L_8007C0BC:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

static void leaf_8007E8DC(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-40;
    c->r[7] = c->r[7] << 16;
    c->r[2] = (uint32_t)32769u << 16;
    c->r[2] = c->r[2] + (uint32_t)29492;
    c->r[7] = (uint32_t)((int32_t)c->r[7] >> 14);
    c->r[7] = c->r[7] + c->r[2];
    c->r[2] = (uint32_t)32783u << 16;
    c->mem_w16((c->r[29] + (uint32_t)24), (uint16_t)c->r[4]);
    c->r[4] = c->r[29] + (uint32_t)24;
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[31]);
    c->mem_w8((c->r[29] + (uint32_t)17), (uint8_t)c->r[6]);
    c->mem_w8((c->r[29] + (uint32_t)16), (uint8_t)c->r[0]);
    c->mem_w16((c->r[29] + (uint32_t)18), (uint16_t)c->r[0]);
    c->mem_w16((c->r[29] + (uint32_t)26), (uint16_t)c->r[5]);
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[0]);
    c->r[5] = c->mem_r32((c->r[7] + (uint32_t)0));
    c->r[6] = c->mem_r32((c->r[2] + (uint32_t)-12456));
    c->r[31] = 0x8007E928u;
    c->r[7] = c->r[29] + (uint32_t)16; func_8007E1B8(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[29] = c->r[29] + (uint32_t)40; return;
    return;
}

static void leaf_8007E998(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->r[4] = c->r[4] << 16;
    c->r[5] = c->r[5] << 16;
    c->r[4] = (uint32_t)((int32_t)c->r[4] >> 16);
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16);
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[31]);
    c->r[31] = 0x8007E9B8u;
    c->r[7] = c->r[0] + (uint32_t)152; func_8007E8DC(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_8007ED5C(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-80;
    c->mem_w32((c->r[29] + (uint32_t)72), c->r[20]);
    c->r[20] = c->r[4] + c->r[0];
    c->r[4] = c->r[29] + (uint32_t)24;
    c->r[5] = c->r[0] + (uint32_t)1;
    c->mem_w32((c->r[29] + (uint32_t)60), c->r[17]);
    c->r[17] = c->r[29] + (uint32_t)40;
    c->r[6] = c->r[17] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)64), c->r[18]);
    c->r[18] = c->r[0] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)56), c->r[16]);
    c->r[16] = (uint32_t)32778u << 16;
    c->r[16] = c->r[16] + (uint32_t)10324;
    c->mem_w32((c->r[29] + (uint32_t)76), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)68), c->r[19]);
    c->r[2] = c->mem_r32((c->r[16] + (uint32_t)12));
    c->r[19] = c->r[0] + (uint32_t)104;
    c->r[31] = 0x8007EDA8u;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[2]); func_800793C4(c);
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[29] + (uint32_t)40));
    c->r[5] = c->r[0] + (uint32_t)80;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]);
    c->r[7] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[31] = 0x8007EDC0u;
    c->r[6] = c->r[18] + c->r[0]; func_80079374(c);
    c->r[4] = c->r[29] + (uint32_t)24;
    c->r[5] = c->r[0] + (uint32_t)2;
    c->r[2] = c->mem_r32((c->r[16] + (uint32_t)16));
    c->r[3] = c->mem_r32((c->r[16] + (uint32_t)20));
    c->r[6] = c->r[17] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[2]);
    c->r[31] = 0x8007EDE0u;
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[3]); func_800793C4(c);
    c->r[17] = c->r[2] + c->r[0];
    c->r[6] = c->r[20] ^ c->r[18];
  L_8007EDE8:;
    c->r[6] = (uint32_t)(c->r[6] < (uint32_t)1);
    c->r[6] = c->r[6] << 1;
    c->r[3] = c->r[18] << 2;
    c->r[16] = c->r[29] + (uint32_t)48;
    c->r[5] = c->r[16] + c->r[3];
    c->r[2] = c->r[29] + c->r[3];
    c->mem_w32((c->r[5] + (uint32_t)0), c->r[19]);
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)40));
    c->r[5] = (uint32_t)(int16_t)c->mem_r16((c->r[5] + (uint32_t)0));
    c->r[19] = c->r[19] + (uint32_t)24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]);
    c->r[7] = c->mem_r32((c->r[2] + (uint32_t)24));
    c->r[31] = 0x8007EE20u;
    c->r[18] = c->r[18] + (uint32_t)1; func_80079374(c);
    c->r[2] = (uint32_t)((int32_t)c->r[18] < 2);
    { int _t = (c->r[2] != c->r[0]); c->r[6] = c->r[20] ^ c->r[18]; if (_t) goto L_8007EDE8; }
    c->r[4] = c->r[17] << 16;
    c->r[4] = (uint32_t)((int32_t)c->r[4] >> 16);
    c->r[2] = c->r[20] << 2;
    c->r[2] = c->r[16] + c->r[2];
    c->r[5] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)0));
    c->r[6] = c->r[0] + c->r[0];
    c->r[5] = c->r[5] + (uint32_t)4;
    c->r[5] = c->r[5] << 16;
    c->r[31] = 0x8007EE54u;
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16); func_8007E998(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)76));
    c->r[20] = c->mem_r32((c->r[29] + (uint32_t)72));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)68));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)64));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)60));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)56));
    c->r[29] = c->r[29] + (uint32_t)80; return;
    return;
}

static void leaf_8007EE74(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-96;
    c->mem_w32((c->r[29] + (uint32_t)84), c->r[19]);
    c->r[19] = c->r[4] + c->r[0];
    c->r[4] = c->r[29] + (uint32_t)24;
    c->r[5] = c->r[0] + (uint32_t)3;
    c->r[6] = c->r[29] + (uint32_t)40;
    c->mem_w32((c->r[29] + (uint32_t)72), c->r[16]);
    c->r[16] = c->r[0] + c->r[0];
    c->r[2] = (uint32_t)32778u << 16;
    c->r[2] = c->r[2] + (uint32_t)10324;
    c->mem_w32((c->r[29] + (uint32_t)92), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)88), c->r[20]);
    c->mem_w32((c->r[29] + (uint32_t)80), c->r[18]);
    c->mem_w32((c->r[29] + (uint32_t)76), c->r[17]);
    c->r[3] = c->mem_r32((c->r[2] + (uint32_t)24));
    c->r[7] = c->mem_r32((c->r[2] + (uint32_t)28));
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)32));
    c->r[17] = c->r[0] + (uint32_t)80;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[3]);
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[7]);
    c->r[31] = 0x8007EECCu;
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[2]); func_800793C4(c);
    c->r[20] = c->r[2] + c->r[0];
    c->r[6] = c->r[19] ^ c->r[16];
  L_8007EED4:;
    c->r[6] = (uint32_t)(c->r[6] < (uint32_t)1);
    c->r[6] = c->r[6] << 1;
    c->r[7] = c->r[16] << 2;
    c->r[18] = c->r[29] + (uint32_t)56;
    c->r[3] = c->r[18] + c->r[7];
    c->r[2] = c->r[29] + c->r[7];
    c->mem_w32((c->r[3] + (uint32_t)0), c->r[17]);
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)40));
    c->r[5] = (uint32_t)(int16_t)c->mem_r16((c->r[3] + (uint32_t)0));
    c->r[17] = c->r[17] + (uint32_t)24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]);
    c->r[7] = c->mem_r32((c->r[2] + (uint32_t)24));
    c->r[31] = 0x8007EF0Cu;
    c->r[16] = c->r[16] + (uint32_t)1; func_80079374(c);
    c->r[2] = (uint32_t)((int32_t)c->r[16] < 3);
    { int _t = (c->r[2] != c->r[0]); c->r[6] = c->r[19] ^ c->r[16]; if (_t) goto L_8007EED4; }
    c->r[4] = c->r[20] << 16;
    c->r[4] = (uint32_t)((int32_t)c->r[4] >> 16);
    c->r[2] = c->r[19] << 2;
    c->r[2] = c->r[18] + c->r[2];
    c->r[5] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)0));
    c->r[6] = c->r[0] + (uint32_t)1;
    c->r[5] = c->r[5] + (uint32_t)4;
    c->r[5] = c->r[5] << 16;
    c->r[31] = 0x8007EF40u;
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16); func_8007E998(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)92));
    c->r[20] = c->mem_r32((c->r[29] + (uint32_t)88));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)84));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)80));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)76));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)72));
    c->r[29] = c->r[29] + (uint32_t)96; return;
    return;
}

static void leaf_8007EF60(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-96;
    c->mem_w32((c->r[29] + (uint32_t)88), c->r[20]);
    c->r[20] = c->r[4] + c->r[0];
    c->r[4] = c->r[29] + (uint32_t)24;
    c->r[5] = c->r[0] + (uint32_t)1;
    c->mem_w32((c->r[29] + (uint32_t)76), c->r[17]);
    c->r[17] = c->r[29] + (uint32_t)40;
    c->r[6] = c->r[17] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)80), c->r[18]);
    c->r[18] = c->r[0] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)72), c->r[16]);
    c->r[16] = (uint32_t)32778u << 16;
    c->r[16] = c->r[16] + (uint32_t)10324;
    c->mem_w32((c->r[29] + (uint32_t)92), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)84), c->r[19]);
    c->r[2] = c->mem_r32((c->r[16] + (uint32_t)36));
    c->r[19] = c->r[0] + (uint32_t)104;
    c->r[31] = 0x8007EFACu;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[2]); func_800793C4(c);
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[29] + (uint32_t)40));
    c->r[5] = c->r[0] + (uint32_t)80;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]);
    c->r[7] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[31] = 0x8007EFC4u;
    c->r[6] = c->r[18] + c->r[0]; func_80079374(c);
    c->r[4] = c->r[29] + (uint32_t)24;
    c->r[5] = c->r[0] + (uint32_t)2;
    c->r[2] = c->mem_r32((c->r[16] + (uint32_t)40));
    c->r[3] = c->mem_r32((c->r[16] + (uint32_t)44));
    c->r[6] = c->r[17] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[2]);
    c->r[31] = 0x8007EFE4u;
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[3]); func_800793C4(c);
    c->r[17] = c->r[2] + c->r[0];
    c->r[6] = c->r[20] ^ c->r[18];
  L_8007EFEC:;
    c->r[6] = (uint32_t)(c->r[6] < (uint32_t)1);
    c->r[6] = c->r[6] << 1;
    c->r[3] = c->r[18] << 2;
    c->r[16] = c->r[29] + (uint32_t)56;
    c->r[5] = c->r[16] + c->r[3];
    c->r[2] = c->r[29] + c->r[3];
    c->mem_w32((c->r[5] + (uint32_t)0), c->r[19]);
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)40));
    c->r[5] = (uint32_t)(int16_t)c->mem_r16((c->r[5] + (uint32_t)0));
    c->r[19] = c->r[19] + (uint32_t)24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]);
    c->r[7] = c->mem_r32((c->r[2] + (uint32_t)24));
    c->r[31] = 0x8007F024u;
    c->r[18] = c->r[18] + (uint32_t)1; func_80079374(c);
    c->r[2] = (uint32_t)((int32_t)c->r[18] < 2);
    { int _t = (c->r[2] != c->r[0]); c->r[6] = c->r[20] ^ c->r[18]; if (_t) goto L_8007EFEC; }
    c->r[4] = c->r[17] << 16;
    c->r[4] = (uint32_t)((int32_t)c->r[4] >> 16);
    c->r[2] = c->r[20] << 2;
    c->r[2] = c->r[16] + c->r[2];
    c->r[5] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)0));
    c->r[6] = c->r[0] + c->r[0];
    c->r[5] = c->r[5] + (uint32_t)4;
    c->r[5] = c->r[5] << 16;
    c->r[31] = 0x8007F058u;
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16); func_8007E998(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)92));
    c->r[20] = c->mem_r32((c->r[29] + (uint32_t)88));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)84));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)80));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)76));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)72));
    c->r[29] = c->r[29] + (uint32_t)96; return;
    return;
}

static void leaf_8007F078(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->r[4] = c->r[0] + (uint32_t)118;
    c->r[5] = c->r[0] + (uint32_t)200;
    c->r[6] = c->r[0] + c->r[0];
    c->r[7] = (uint32_t)32769u << 16;
    c->r[7] = c->r[7] + (uint32_t)29348;
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[16]);
    c->r[31] = 0x8007F0A0u;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]); func_80078988(c);
    c->r[4] = c->r[0] + (uint32_t)130;
    c->r[5] = c->r[0] + (uint32_t)200;
    c->r[16] = (uint32_t)32778u << 16;
    c->r[16] = c->r[16] + (uint32_t)10324;
    c->r[7] = c->mem_r32((c->r[16] + (uint32_t)164));
    c->r[6] = c->r[0] + (uint32_t)6;
    c->r[31] = 0x8007F0C0u;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]); func_80079324(c);
    c->r[4] = c->r[0] + (uint32_t)198;
    c->r[5] = c->r[0] + (uint32_t)200;
    c->r[6] = c->r[0] + c->r[0];
    c->r[7] = (uint32_t)32769u << 16;
    c->r[7] = c->r[7] + (uint32_t)29352;
    c->r[31] = 0x8007F0DCu;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]); func_80078988(c);
    c->r[4] = c->r[0] + (uint32_t)210;
    c->r[5] = c->r[0] + (uint32_t)200;
    c->r[7] = c->mem_r32((c->r[16] + (uint32_t)172));
    c->r[6] = c->r[0] + (uint32_t)6;
    c->r[31] = 0x8007F0F4u;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]); func_80079324(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

static void leaf_8007F104(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-96;
    c->r[4] = c->r[0] + (uint32_t)38;
    c->r[5] = c->r[0] + (uint32_t)200;
    c->r[6] = c->r[0] + c->r[0];
    c->r[7] = (uint32_t)32769u << 16;
    c->r[7] = c->r[7] + (uint32_t)29356;
    c->mem_w32((c->r[29] + (uint32_t)92), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)88), c->r[20]);
    c->mem_w32((c->r[29] + (uint32_t)84), c->r[19]);
    c->mem_w32((c->r[29] + (uint32_t)80), c->r[18]);
    c->mem_w32((c->r[29] + (uint32_t)76), c->r[17]);
    c->mem_w32((c->r[29] + (uint32_t)72), c->r[16]);
    c->r[31] = 0x8007F13Cu;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]); func_80078988(c);
    c->r[4] = c->r[0] + (uint32_t)50;
    c->r[5] = c->r[0] + (uint32_t)200;
    c->r[6] = c->r[0] + (uint32_t)6;
    c->r[17] = c->r[0] + c->r[0];
    c->r[16] = (uint32_t)32778u << 16;
    c->r[16] = c->r[16] + (uint32_t)10324;
    c->r[7] = c->mem_r32((c->r[16] + (uint32_t)160));
    c->r[18] = c->r[0] + (uint32_t)76;
    c->r[31] = 0x8007F164u;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]); func_80079324(c);
    c->r[4] = c->r[0] + (uint32_t)32;
    c->r[5] = c->r[4] + c->r[0];
    c->r[7] = c->mem_r32((c->r[16] + (uint32_t)48));
    c->r[6] = c->r[17] + c->r[0];
    c->r[31] = 0x8007F17Cu;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]); func_80079374(c);
    c->r[4] = c->r[29] + (uint32_t)24;
    c->r[5] = c->r[0] + (uint32_t)4;
    c->r[2] = c->mem_r32((c->r[16] + (uint32_t)52));
    c->r[3] = c->mem_r32((c->r[16] + (uint32_t)56));
    c->r[7] = c->mem_r32((c->r[16] + (uint32_t)60));
    c->r[8] = c->mem_r32((c->r[16] + (uint32_t)64));
    c->r[6] = c->r[29] + (uint32_t)40;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[2]);
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[3]);
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[7]);
    c->r[31] = 0x8007F1ACu;
    c->mem_w32((c->r[29] + (uint32_t)36), c->r[8]); func_800793C4(c);
    c->r[20] = c->r[2] + c->r[0];
  L_8007F1B0:;
    c->r[19] = (uint32_t)32780u << 16;
    c->r[7] = c->r[17] << 2;
    c->r[16] = c->r[29] + (uint32_t)56;
    c->r[3] = c->r[16] + c->r[7];
    c->r[6] = (uint32_t)c->mem_r8((c->r[19] + (uint32_t)-2040));
    c->r[2] = c->r[29] + c->r[7];
    c->mem_w32((c->r[3] + (uint32_t)0), c->r[18]);
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)40));
    c->r[5] = (uint32_t)(int16_t)c->mem_r16((c->r[3] + (uint32_t)0));
    c->r[18] = c->r[18] + (uint32_t)24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]);
    c->r[7] = c->mem_r32((c->r[2] + (uint32_t)24));
    c->r[6] = c->r[6] ^ c->r[17];
    c->r[6] = (uint32_t)(c->r[6] < (uint32_t)1);
    c->r[31] = 0x8007F1F0u;
    c->r[6] = c->r[6] << 3; func_80079374(c);
    c->r[17] = c->r[17] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[17] < 4);
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[20] << 16; if (_t) goto L_8007F1B0; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[19] + (uint32_t)-2040));
    c->r[4] = (uint32_t)((int32_t)c->r[4] >> 16);
    c->r[2] = c->r[2] << 2;
    c->r[2] = c->r[16] + c->r[2];
    c->r[5] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)0));
    c->r[6] = c->r[0] + c->r[0];
    c->r[5] = c->r[5] + (uint32_t)4;
    c->r[5] = c->r[5] << 16;
    c->r[31] = 0x8007F228u;
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16); func_8007E998(c);
    c->r[31] = 0x8007F230u;
     func_8007FC24(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)92));
    c->r[20] = c->mem_r32((c->r[29] + (uint32_t)88));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)84));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)80));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)76));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)72));
    c->r[29] = c->r[29] + (uint32_t)96; return;
    return;
}

static void leaf_8007F250(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-72;
    c->r[4] = c->r[0] + (uint32_t)48;
    c->r[5] = c->r[4] + c->r[0];
    c->r[6] = c->r[0] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)52), c->r[17]);
    c->r[17] = c->r[6] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)48), c->r[16]);
    c->r[16] = (uint32_t)32778u << 16;
    c->r[16] = c->r[16] + (uint32_t)10324;
    c->mem_w32((c->r[29] + (uint32_t)64), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)60), c->r[19]);
    c->mem_w32((c->r[29] + (uint32_t)56), c->r[18]);
    c->r[7] = c->mem_r32((c->r[16] + (uint32_t)52));
    c->r[19] = (uint32_t)32780u << 16;
    c->r[31] = 0x8007F290u;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]); func_80079374(c);
    c->r[2] = c->r[0] + (uint32_t)96;
    c->mem_w16((c->r[29] + (uint32_t)40), (uint16_t)c->r[2]);
    c->r[3] = c->mem_r32((c->r[16] + (uint32_t)68));
    c->r[4] = c->mem_r32((c->r[16] + (uint32_t)80));
    c->r[2] = c->r[0] + (uint32_t)128;
    c->mem_w16((c->r[29] + (uint32_t)42), (uint16_t)c->r[2]);
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[3]);
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[4]);
    c->r[2] = c->r[17] << 1;
  L_8007F2B4:;
    c->r[18] = c->r[29] + (uint32_t)40;
    c->r[2] = c->r[18] + c->r[2];
    c->r[4] = c->r[0] + (uint32_t)64;
    c->r[5] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)0));
    c->r[2] = c->r[17] << 2;
    c->r[16] = c->r[29] + (uint32_t)24;
    c->r[6] = (uint32_t)c->mem_r8((c->r[19] + (uint32_t)-2040));
    c->r[2] = c->r[16] + c->r[2];
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]);
    c->r[7] = c->mem_r32((c->r[2] + (uint32_t)0));
    c->r[6] = c->r[6] ^ c->r[17];
    c->r[6] = (uint32_t)(c->r[6] < (uint32_t)1);
    c->r[31] = 0x8007F2ECu;
    c->r[6] = c->r[6] << 3; func_80079374(c);
    c->r[17] = c->r[17] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[17] < 2);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[17] << 1; if (_t) goto L_8007F2B4; }
    c->r[4] = c->r[0] + (uint32_t)44;
    c->r[6] = c->r[0] + c->r[0];
    c->r[2] = (uint32_t)32780u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-2040));
    c->r[17] = c->r[6] + c->r[0];
    c->r[2] = c->r[2] << 1;
    c->r[2] = c->r[18] + c->r[2];
    c->r[5] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)0));
    c->r[18] = c->r[16] + c->r[0];
    c->r[5] = c->r[5] + (uint32_t)4;
    c->r[5] = c->r[5] << 16;
    c->r[31] = 0x8007F330u;
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16); func_8007E998(c);
    c->r[4] = c->r[0] + (uint32_t)140;
    c->r[5] = c->r[0] + (uint32_t)86;
    c->r[16] = (uint32_t)32778u << 16;
    c->r[16] = c->r[16] + (uint32_t)10324;
    c->r[7] = c->mem_r32((c->r[16] + (uint32_t)72));
    c->r[6] = c->r[0] + c->r[0];
    c->r[31] = 0x8007F350u;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]); func_80079324(c);
    c->r[4] = c->r[0] + (uint32_t)236;
    c->r[5] = c->r[0] + (uint32_t)86;
    c->r[7] = c->mem_r32((c->r[16] + (uint32_t)76));
    c->r[6] = c->r[0] + c->r[0];
    c->r[31] = 0x8007F368u;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]); func_80079324(c);
    c->r[4] = c->r[0] + (uint32_t)172;
    c->r[5] = c->r[0] + (uint32_t)100;
    c->r[6] = c->r[0] + c->r[0];
    c->r[7] = (uint32_t)32769u << 16;
    c->r[7] = c->r[7] + (uint32_t)29360;
    c->r[31] = 0x8007F384u;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]); func_80078988(c);
    c->r[2] = (uint32_t)32784u << 16;
    c->r[16] = c->r[2] + (uint32_t)-20128;
    c->r[2] = (uint32_t)32769u << 16;
    c->r[2] = c->r[2] + (uint32_t)29376;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[2]);
    c->r[2] = (uint32_t)32769u << 16;
    c->r[2] = c->r[2] + (uint32_t)29392;
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[2]);
    c->r[2] = (uint32_t)32769u << 16;
    c->r[2] = c->r[2] + (uint32_t)29408;
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[2]);
  L_8007F3B0:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)1));
    { int _t = (c->r[17] != c->r[2]); c->r[6] = c->r[0] + c->r[0]; if (_t) goto L_8007F3C4; }
    c->r[6] = c->r[0] + (uint32_t)3;
  L_8007F3C4:;
    c->r[4] = c->r[0] + (uint32_t)148;
    c->r[5] = c->r[0] + (uint32_t)99;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]);
    c->r[7] = c->mem_r32((c->r[18] + (uint32_t)0));
    c->r[18] = c->r[18] + (uint32_t)4;
    c->r[31] = 0x8007F3E0u;
    c->r[17] = c->r[17] + (uint32_t)1; func_80079374(c);
    c->r[2] = (uint32_t)((int32_t)c->r[17] < 3);
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[0] + (uint32_t)192; if (_t) goto L_8007F3B0; }
    c->r[5] = c->r[0] + (uint32_t)131;
    c->r[6] = c->r[0] + c->r[0];
    c->r[7] = (uint32_t)32769u << 16;
    c->r[7] = c->r[7] + (uint32_t)29424;
    c->r[31] = 0x8007F404u;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]); func_80079374(c);
    c->r[17] = c->r[0] + c->r[0];
    c->r[2] = (uint32_t)32784u << 16;
    c->r[18] = c->r[2] + (uint32_t)-20128;
    c->r[2] = (uint32_t)32778u << 16;
    c->r[2] = c->r[2] + (uint32_t)10324;
    c->r[3] = c->mem_r32((c->r[2] + (uint32_t)84));
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)88));
    c->r[16] = (uint32_t)148u << 16;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[3]);
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[2]);
  L_8007F42C:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)2));
    { int _t = (c->r[17] != c->r[2]); c->r[6] = c->r[0] + c->r[0]; if (_t) goto L_8007F440; }
    c->r[6] = c->r[0] + (uint32_t)3;
  L_8007F440:;
    c->r[4] = (uint32_t)((int32_t)c->r[16] >> 16);
    c->r[5] = c->r[0] + (uint32_t)131;
    c->r[2] = c->r[17] << 2;
    c->r[2] = c->r[29] + c->r[2];
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]);
    c->r[7] = c->mem_r32((c->r[2] + (uint32_t)24));
    c->r[31] = 0x8007F460u;
    c->r[17] = c->r[17] + (uint32_t)1; func_80079374(c);
    c->r[2] = (uint32_t)72u << 16;
    c->r[16] = c->r[16] + c->r[2];
    c->r[2] = (uint32_t)((int32_t)c->r[17] < 2);
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_8007F42C; }
    c->r[31] = 0x8007F47Cu;
     func_8007FC24(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)64));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)60));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)56));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)52));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)48));
    c->r[29] = c->r[29] + (uint32_t)72; return;
    return;
}

static void leaf_8007F498(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-72;
    c->r[4] = c->r[0] + (uint32_t)48;
    c->r[5] = c->r[4] + c->r[0];
    c->r[6] = c->r[0] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)52), c->r[17]);
    c->r[17] = c->r[6] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)48), c->r[16]);
    c->r[16] = (uint32_t)32778u << 16;
    c->r[16] = c->r[16] + (uint32_t)10324;
    c->mem_w32((c->r[29] + (uint32_t)64), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)60), c->r[19]);
    c->mem_w32((c->r[29] + (uint32_t)56), c->r[18]);
    c->r[7] = c->mem_r32((c->r[16] + (uint32_t)56));
    c->r[18] = (uint32_t)32780u << 16;
    c->r[31] = 0x8007F4D8u;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]); func_80079374(c);
    c->r[2] = c->r[0] + (uint32_t)80;
    c->mem_w16((c->r[29] + (uint32_t)40), (uint16_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)112;
    c->mem_w16((c->r[29] + (uint32_t)42), (uint16_t)c->r[2]);
    c->r[3] = c->mem_r32((c->r[16] + (uint32_t)92));
    c->r[4] = c->mem_r32((c->r[16] + (uint32_t)104));
    c->r[5] = c->mem_r32((c->r[16] + (uint32_t)116));
    c->r[2] = c->r[0] + (uint32_t)144;
    c->mem_w16((c->r[29] + (uint32_t)44), (uint16_t)c->r[2]);
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[3]);
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[4]);
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[5]);
    c->r[2] = c->r[17] << 1;
  L_8007F50C:;
    c->r[16] = c->r[29] + (uint32_t)40;
    c->r[2] = c->r[16] + c->r[2];
    c->r[4] = c->r[0] + (uint32_t)64;
    c->r[5] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)0));
    c->r[2] = c->r[17] << 2;
    c->r[6] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)-2040));
    c->r[2] = c->r[29] + c->r[2];
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]);
    c->r[7] = c->mem_r32((c->r[2] + (uint32_t)24));
    c->r[6] = c->r[6] ^ c->r[17];
    c->r[6] = (uint32_t)(c->r[6] < (uint32_t)1);
    c->r[31] = 0x8007F540u;
    c->r[6] = c->r[6] << 3; func_80079374(c);
    c->r[17] = c->r[17] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[17] < 3);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[17] << 1; if (_t) goto L_8007F50C; }
    c->r[4] = c->r[0] + (uint32_t)44;
    c->r[6] = c->r[0] + c->r[0];
    c->r[2] = (uint32_t)32780u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-2040));
    c->r[17] = c->r[6] + c->r[0];
    c->r[2] = c->r[2] << 1;
    c->r[2] = c->r[16] + c->r[2];
    c->r[5] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)0));
    c->r[16] = (uint32_t)148u << 16;
    c->r[5] = c->r[5] + (uint32_t)4;
    c->r[5] = c->r[5] << 16;
    c->r[31] = 0x8007F584u;
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16); func_8007E998(c);
    c->r[4] = c->r[0] + (uint32_t)215;
    c->r[5] = c->r[0] + (uint32_t)82;
    c->r[6] = c->r[0] + c->r[0];
    c->r[7] = (uint32_t)32769u << 16;
    c->r[7] = c->r[7] + (uint32_t)29424;
    c->r[31] = 0x8007F5A0u;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]); func_80079374(c);
    c->r[2] = (uint32_t)32778u << 16;
    c->r[2] = c->r[2] + (uint32_t)10324;
    c->r[3] = (uint32_t)32784u << 16;
    c->r[4] = c->mem_r32((c->r[2] + (uint32_t)96));
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)100));
    c->r[18] = c->r[3] + (uint32_t)-20128;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[4]);
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[2]);
  L_8007F5C0:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)3));
    { int _t = (c->r[17] != c->r[2]); c->r[6] = c->r[0] + c->r[0]; if (_t) goto L_8007F5D4; }
    c->r[6] = c->r[0] + (uint32_t)3;
  L_8007F5D4:;
    c->r[4] = (uint32_t)((int32_t)c->r[16] >> 16);
    c->r[5] = c->r[0] + (uint32_t)80;
    c->r[2] = c->r[17] << 2;
    c->r[2] = c->r[29] + c->r[2];
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]);
    c->r[7] = c->mem_r32((c->r[2] + (uint32_t)24));
    c->r[31] = 0x8007F5F4u;
    c->r[17] = c->r[17] + (uint32_t)1; func_80079374(c);
    c->r[2] = (uint32_t)80u << 16;
    c->r[16] = c->r[16] + c->r[2];
    c->r[2] = (uint32_t)((int32_t)c->r[17] < 2);
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[0] + (uint32_t)152; if (_t) goto L_8007F5C0; }
    c->r[5] = c->r[0] + (uint32_t)116;
    c->r[6] = c->r[0] + c->r[0];
    c->r[17] = c->r[6] + c->r[0];
    c->r[19] = (uint32_t)32769u << 16;
    c->r[16] = (uint32_t)32778u << 16;
    c->r[16] = c->r[16] + (uint32_t)10324;
    c->r[7] = c->mem_r32((c->r[16] + (uint32_t)108));
    c->r[18] = (uint32_t)180u << 16;
    c->r[31] = 0x8007F630u;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]); func_80079324(c);
    c->r[4] = c->r[0] + (uint32_t)264;
    c->r[5] = c->r[0] + (uint32_t)116;
    c->r[7] = c->mem_r32((c->r[16] + (uint32_t)112));
    c->r[6] = c->r[0] + c->r[0];
    c->r[31] = 0x8007F648u;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]); func_80079324(c);
    c->r[2] = (uint32_t)32784u << 16;
    c->r[16] = c->r[2] + (uint32_t)-20128;
  L_8007F650:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)4));
    { int _t = (c->r[17] != c->r[2]); c->r[6] = c->r[0] + c->r[0]; if (_t) goto L_8007F664; }
    c->r[6] = c->r[0] + (uint32_t)3;
  L_8007F664:;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]);
    c->r[4] = (uint32_t)((int32_t)c->r[18] >> 16);
    c->r[5] = c->r[0] + (uint32_t)116;
    c->r[31] = 0x8007F678u;
    c->r[7] = c->r[19] + (uint32_t)29428; func_80078988(c);
    c->r[2] = (uint32_t)8u << 16;
    c->r[18] = c->r[18] + c->r[2];
    c->r[17] = c->r[17] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[17] < 10);
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[0] + (uint32_t)152; if (_t) goto L_8007F650; }
    c->r[5] = c->r[0] + (uint32_t)148;
    c->r[6] = c->r[0] + c->r[0];
    c->r[17] = c->r[6] + c->r[0];
    c->r[19] = (uint32_t)32769u << 16;
    c->r[16] = (uint32_t)32778u << 16;
    c->r[16] = c->r[16] + (uint32_t)10324;
    c->r[7] = c->mem_r32((c->r[16] + (uint32_t)108));
    c->r[18] = (uint32_t)180u << 16;
    c->r[31] = 0x8007F6B8u;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]); func_80079324(c);
    c->r[4] = c->r[0] + (uint32_t)264;
    c->r[5] = c->r[0] + (uint32_t)148;
    c->r[7] = c->mem_r32((c->r[16] + (uint32_t)112));
    c->r[6] = c->r[0] + c->r[0];
    c->r[31] = 0x8007F6D0u;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]); func_80079324(c);
    c->r[2] = (uint32_t)32784u << 16;
    c->r[16] = c->r[2] + (uint32_t)-20128;
  L_8007F6D8:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)5));
    { int _t = (c->r[17] != c->r[2]); c->r[6] = c->r[0] + c->r[0]; if (_t) goto L_8007F6EC; }
    c->r[6] = c->r[0] + (uint32_t)3;
  L_8007F6EC:;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]);
    c->r[4] = (uint32_t)((int32_t)c->r[18] >> 16);
    c->r[5] = c->r[0] + (uint32_t)148;
    c->r[31] = 0x8007F700u;
    c->r[7] = c->r[19] + (uint32_t)29428; func_80078988(c);
    c->r[2] = (uint32_t)8u << 16;
    c->r[18] = c->r[18] + c->r[2];
    c->r[17] = c->r[17] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[17] < 10);
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_8007F6D8; }
    c->r[31] = 0x8007F720u;
     func_8007FC24(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)64));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)60));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)56));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)52));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)48));
    c->r[29] = c->r[29] + (uint32_t)72; return;
    return;
}

static void leaf_8007F73C(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-80;
    c->r[4] = c->r[0] + (uint32_t)48;
    c->r[5] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)56), c->r[16]);
    c->r[16] = (uint32_t)32778u << 16;
    c->r[16] = c->r[16] + (uint32_t)10324;
    c->mem_w32((c->r[29] + (uint32_t)76), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)72), c->r[20]);
    c->mem_w32((c->r[29] + (uint32_t)68), c->r[19]);
    c->mem_w32((c->r[29] + (uint32_t)64), c->r[18]);
    c->mem_w32((c->r[29] + (uint32_t)60), c->r[17]);
    c->r[7] = c->mem_r32((c->r[16] + (uint32_t)60));
    c->r[6] = c->r[0] + c->r[0];
    c->r[31] = 0x8007F778u;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]); func_80079374(c);
    c->r[4] = c->mem_r32((c->r[16] + (uint32_t)60));
    c->r[31] = 0x8007F784u;
    c->r[20] = c->r[0] + c->r[0]; func_80079528(c);
    c->r[4] = c->r[0] + (uint32_t)40;
    c->r[5] = c->r[0] + (uint32_t)44;
    c->r[2] = c->r[2] << 3;
    c->r[2] = c->r[2] + (uint32_t)16;
    c->r[2] = c->r[2] << 16;
    c->r[6] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[7] = c->r[0] + (uint32_t)24;
    c->r[31] = 0x8007F7A8u;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]); func_8007FCC8(c);
    c->r[2] = c->r[0] + (uint32_t)92;
    c->mem_w16((c->r[29] + (uint32_t)40), (uint16_t)c->r[2]);
    c->r[3] = c->mem_r32((c->r[16] + (uint32_t)120));
    c->r[4] = c->mem_r32((c->r[16] + (uint32_t)124));
    c->r[2] = c->r[0] + (uint32_t)132;
    c->mem_w16((c->r[29] + (uint32_t)42), (uint16_t)c->r[2]);
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[3]);
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[4]);
    c->r[2] = c->r[20] << 2;
  L_8007F7CC:;
    c->r[19] = c->r[29] + (uint32_t)24;
    c->r[19] = c->r[19] + c->r[2];
    c->r[4] = c->mem_r32((c->r[19] + (uint32_t)0));
    c->r[31] = 0x8007F7E0u;
    c->r[16] = c->r[0] + (uint32_t)152; func_80079528(c);
    c->r[18] = c->r[2] << 3;
    c->r[16] = c->r[16] - c->r[18];
    c->r[4] = c->r[16] << 16;
    c->r[4] = (uint32_t)((int32_t)c->r[4] >> 16);
    c->r[2] = c->r[20] << 1;
    c->r[17] = c->r[29] + (uint32_t)40;
    c->r[17] = c->r[17] + c->r[2];
    c->r[5] = (uint32_t)(int16_t)c->mem_r16((c->r[17] + (uint32_t)0));
    c->r[6] = c->r[0] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]);
    c->r[7] = c->mem_r32((c->r[19] + (uint32_t)0));
    c->r[31] = 0x8007F814u;
    c->r[20] = c->r[20] + (uint32_t)1; func_80079374(c);
    c->r[16] = c->r[16] + (uint32_t)-8;
    c->r[16] = c->r[16] << 16;
    c->r[4] = (uint32_t)((int32_t)c->r[16] >> 16);
    c->r[18] = c->r[18] + (uint32_t)48;
    c->r[18] = c->r[18] << 16;
    c->r[6] = (uint32_t)((int32_t)c->r[18] >> 16);
    c->r[5] = (uint32_t)c->mem_r16((c->r[17] + (uint32_t)0));
    c->r[7] = c->r[0] + (uint32_t)24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]);
    c->r[5] = c->r[5] + (uint32_t)-4;
    c->r[5] = c->r[5] << 16;
    c->r[31] = 0x8007F848u;
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16); func_8007FCC8(c);
    c->r[2] = (uint32_t)((int32_t)c->r[20] < 2);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[20] << 2; if (_t) goto L_8007F7CC; }
    c->r[17] = c->r[29] + (uint32_t)48;
    c->r[4] = c->r[17] + c->r[0];
    c->r[16] = (uint32_t)32769u << 16;
    c->r[16] = c->r[16] + (uint32_t)29432;
    c->r[2] = (uint32_t)32782u << 16;
    c->r[6] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)32370));
    c->r[5] = c->r[16] + c->r[0];
    c->r[31] = 0x8007F878u;
    c->r[6] = c->r[6] + (uint32_t)-8; func_8009B0C0(c);
    c->r[4] = c->r[0] + (uint32_t)168;
    c->r[5] = c->r[0] + (uint32_t)94;
    c->r[6] = c->r[0] + (uint32_t)3;
    c->r[7] = c->r[17] + c->r[0];
    c->r[31] = 0x8007F890u;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]); func_80079374(c);
    c->r[4] = c->r[17] + c->r[0];
    c->r[2] = (uint32_t)32782u << 16;
    c->r[6] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)32368));
    c->r[31] = 0x8007F8A4u;
    c->r[5] = c->r[16] + c->r[0]; func_8009B0C0(c);
    c->r[4] = c->r[0] + (uint32_t)168;
    c->r[5] = c->r[0] + (uint32_t)134;
    c->r[6] = c->r[0] + (uint32_t)3;
    c->r[7] = c->r[17] + c->r[0];
    c->r[31] = 0x8007F8BCu;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]); func_80079374(c);
    c->r[4] = c->r[0] + (uint32_t)100;
    c->r[5] = c->r[0] + (uint32_t)193;
    c->r[6] = c->r[0] + (uint32_t)192;
    c->r[7] = c->r[0] + (uint32_t)21;
    c->r[2] = c->r[0] + (uint32_t)1;
    c->r[31] = 0x8007F8D8u;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[2]); func_8007FCC8(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)76));
    c->r[20] = c->mem_r32((c->r[29] + (uint32_t)72));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)68));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)64));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)60));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)56));
    c->r[29] = c->r[29] + (uint32_t)80; return;
    return;
}

static void leaf_8007F8F8(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-64;
    c->r[4] = c->r[0] + (uint32_t)48;
    c->r[5] = c->r[0] + (uint32_t)32;
    c->r[6] = c->r[0] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)52), c->r[17]);
    c->r[17] = c->r[6] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)48), c->r[16]);
    c->r[16] = (uint32_t)32778u << 16;
    c->r[16] = c->r[16] + (uint32_t)10324;
    c->mem_w32((c->r[29] + (uint32_t)60), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)56), c->r[18]);
    c->r[7] = c->mem_r32((c->r[16] + (uint32_t)64));
    c->r[18] = (uint32_t)32780u << 16;
    c->r[31] = 0x8007F934u;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]); func_80079374(c);
    c->r[2] = c->r[0] + (uint32_t)64;
    c->mem_w16((c->r[29] + (uint32_t)40), (uint16_t)c->r[2]);
    c->r[3] = c->mem_r32((c->r[16] + (uint32_t)128));
    c->r[4] = c->mem_r32((c->r[16] + (uint32_t)140));
    c->r[2] = c->r[0] + (uint32_t)88;
    c->mem_w16((c->r[29] + (uint32_t)42), (uint16_t)c->r[2]);
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[3]);
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[4]);
    c->r[2] = c->r[17] << 1;
  L_8007F958:;
    c->r[16] = c->r[29] + (uint32_t)40;
    c->r[2] = c->r[16] + c->r[2];
    c->r[4] = c->r[0] + (uint32_t)64;
    c->r[5] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)0));
    c->r[2] = c->r[17] << 2;
    c->r[6] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)-2040));
    c->r[2] = c->r[29] + c->r[2];
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]);
    c->r[7] = c->mem_r32((c->r[2] + (uint32_t)24));
    c->r[6] = c->r[6] ^ c->r[17];
    c->r[6] = (uint32_t)(c->r[6] < (uint32_t)1);
    c->r[31] = 0x8007F98Cu;
    c->r[6] = c->r[6] << 3; func_80079374(c);
    c->r[17] = c->r[17] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[17] < 2);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[17] << 1; if (_t) goto L_8007F958; }
    c->r[4] = c->r[0] + (uint32_t)44;
    c->r[6] = c->r[0] + c->r[0];
    c->r[2] = (uint32_t)32780u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-2040));
    c->r[17] = c->r[6] + c->r[0];
    c->r[2] = c->r[2] << 1;
    c->r[2] = c->r[16] + c->r[2];
    c->r[5] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)0));
    c->r[16] = (uint32_t)196u << 16;
    c->r[5] = c->r[5] + (uint32_t)4;
    c->r[5] = c->r[5] << 16;
    c->r[31] = 0x8007F9D0u;
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16); func_8007E998(c);
    c->r[4] = c->r[0] + (uint32_t)224;
    c->r[6] = c->r[0] + c->r[0];
    c->r[7] = (uint32_t)32769u << 16;
    c->r[5] = (uint32_t)c->mem_r16((c->r[29] + (uint32_t)40));
    c->r[7] = c->r[7] + (uint32_t)29424;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]);
    c->r[5] = c->r[5] + (uint32_t)3;
    c->r[5] = c->r[5] << 16;
    c->r[31] = 0x8007F9F8u;
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16); func_80079374(c);
    c->r[2] = (uint32_t)32778u << 16;
    c->r[2] = c->r[2] + (uint32_t)10324;
    c->r[3] = (uint32_t)32784u << 16;
    c->r[4] = c->mem_r32((c->r[2] + (uint32_t)132));
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)136));
    c->r[18] = c->r[3] + (uint32_t)-20128;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[4]);
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[2]);
  L_8007FA18:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)7));
    { int _t = (c->r[17] != c->r[2]); c->r[6] = c->r[0] + c->r[0]; if (_t) goto L_8007FA2C; }
    c->r[6] = c->r[0] + (uint32_t)3;
  L_8007FA2C:;
    c->r[4] = (uint32_t)((int32_t)c->r[16] >> 16);
    c->r[2] = c->r[17] << 2;
    c->r[17] = c->r[17] + (uint32_t)1;
    c->r[5] = (uint32_t)c->mem_r16((c->r[29] + (uint32_t)40));
    c->r[2] = c->r[29] + c->r[2];
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]);
    c->r[7] = c->mem_r32((c->r[2] + (uint32_t)24));
    c->r[5] = c->r[5] + (uint32_t)3;
    c->r[5] = c->r[5] << 16;
    c->r[31] = 0x8007FA58u;
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16); func_80079374(c);
    c->r[2] = (uint32_t)48u << 16;
    c->r[16] = c->r[16] + c->r[2];
    c->r[2] = (uint32_t)((int32_t)c->r[17] < 2);
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[0] + (uint32_t)224; if (_t) goto L_8007FA18; }
    c->r[6] = c->r[0] + c->r[0];
    c->r[7] = (uint32_t)32769u << 16;
    c->r[7] = c->r[7] + (uint32_t)29424;
    c->r[17] = c->r[6] + c->r[0];
    c->r[5] = (uint32_t)c->mem_r16((c->r[29] + (uint32_t)42));
    c->r[16] = (uint32_t)204u << 16;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]);
    c->r[5] = c->r[5] + (uint32_t)3;
    c->r[5] = c->r[5] << 16;
    c->r[31] = 0x8007FA98u;
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16); func_80079374(c);
    c->r[2] = (uint32_t)32784u << 16;
    c->r[18] = c->r[2] + (uint32_t)-20128;
    c->r[2] = (uint32_t)32769u << 16;
    c->r[2] = c->r[2] + (uint32_t)29436;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[2]);
    c->r[2] = (uint32_t)32769u << 16;
    c->r[2] = c->r[2] + (uint32_t)29440;
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[2]);
  L_8007FAB8:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)6));
    { int _t = (c->r[17] != c->r[2]); c->r[6] = c->r[0] + c->r[0]; if (_t) goto L_8007FACC; }
    c->r[6] = c->r[0] + (uint32_t)3;
  L_8007FACC:;
    c->r[4] = (uint32_t)((int32_t)c->r[16] >> 16);
    c->r[2] = c->r[17] << 2;
    c->r[17] = c->r[17] + (uint32_t)1;
    c->r[5] = (uint32_t)c->mem_r16((c->r[29] + (uint32_t)42));
    c->r[2] = c->r[29] + c->r[2];
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]);
    c->r[7] = c->mem_r32((c->r[2] + (uint32_t)24));
    c->r[5] = c->r[5] + (uint32_t)3;
    c->r[5] = c->r[5] << 16;
    c->r[31] = 0x8007FAF8u;
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16); func_80079374(c);
    c->r[2] = (uint32_t)40u << 16;
    c->r[16] = c->r[16] + c->r[2];
    c->r[2] = (uint32_t)((int32_t)c->r[17] < 2);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)32784u << 16; if (_t) goto L_8007FAB8; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-20122));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)32778u << 16; if (_t) goto L_8007FB44; }
    c->r[2] = c->r[2] + (uint32_t)10324;
    c->r[3] = c->mem_r32((c->r[2] + (uint32_t)144));
    c->r[4] = c->mem_r32((c->r[2] + (uint32_t)148));
    c->r[5] = c->mem_r32((c->r[2] + (uint32_t)152));
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)156));
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[3]);
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[4]);
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[5]);
    c->mem_w32((c->r[29] + (uint32_t)36), c->r[2]); goto L_8007FB68;
  L_8007FB44:;
    c->r[2] = c->r[2] + (uint32_t)10324;
    c->r[3] = c->mem_r32((c->r[2] + (uint32_t)144));
    c->r[4] = c->mem_r32((c->r[2] + (uint32_t)148));
    c->r[5] = c->mem_r32((c->r[2] + (uint32_t)152));
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)156));
    c->mem_w32((c->r[29] + (uint32_t)36), c->r[3]);
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[4]);
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[5]);
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[2]);
  L_8007FB68:;
    c->r[4] = c->r[0] + (uint32_t)182;
    c->r[5] = c->r[0] + (uint32_t)110;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]);
    c->r[7] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[31] = 0x8007FB80u;
    c->r[6] = c->r[0] + c->r[0]; func_80079324(c);
    c->r[4] = c->r[0] + (uint32_t)200;
    c->r[5] = c->r[0] + (uint32_t)134;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]);
    c->r[7] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[31] = 0x8007FB98u;
    c->r[6] = c->r[0] + c->r[0]; func_80079324(c);
    c->r[4] = c->r[0] + (uint32_t)192;
    c->r[5] = c->r[0] + (uint32_t)164;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]);
    c->r[7] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[31] = 0x8007FBB0u;
    c->r[6] = c->r[0] + c->r[0]; func_80079324(c);
    c->r[4] = c->mem_r32((c->r[29] + (uint32_t)36));
    c->r[31] = 0x8007FBBCu;
     func_80079528(c);
    c->r[2] = c->r[2] << 3;
    c->r[4] = c->r[0] + (uint32_t)120;
    c->r[4] = c->r[4] - c->r[2];
    c->r[4] = c->r[4] << 16;
    c->r[4] = (uint32_t)((int32_t)c->r[4] >> 16);
    c->r[5] = c->r[0] + (uint32_t)136;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]);
    c->r[7] = c->mem_r32((c->r[29] + (uint32_t)36));
    c->r[31] = 0x8007FBE4u;
    c->r[6] = c->r[0] + c->r[0]; func_80079324(c);
    c->r[5] = c->r[0] + (uint32_t)160;
    c->r[6] = c->r[0] + (uint32_t)146;
    c->r[7] = c->r[0] + c->r[0];
    c->r[2] = (uint32_t)32783u << 16;
    c->r[4] = c->mem_r32((c->r[2] + (uint32_t)-12456));
    c->r[2] = c->r[0] + (uint32_t)225;
    c->r[31] = 0x8007FC04u;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[2]); func_8007E938(c);
    c->r[31] = 0x8007FC0Cu;
     func_8007FC24(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)60));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)56));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)52));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)48));
    c->r[29] = c->r[29] + (uint32_t)64; return;
    return;
}

static void leaf_8007FD54(Core* c) {
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)408));
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->r[2] = c->r[2] >> 2;
    c->r[2] = c->r[2] & 1u;
    { int _t = (c->r[2] == c->r[0]); c->mem_w32((c->r[29] + (uint32_t)24), c->r[31]); if (_t) goto L_8007FD84; }
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]);
    c->r[4] = c->r[0] + (uint32_t)160;
    c->r[5] = c->r[0] + (uint32_t)180;
    c->r[6] = c->r[0] + c->r[0]; goto L_8007FD94;
  L_8007FD84:;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]);
    c->r[4] = c->r[0] + (uint32_t)160;
    c->r[5] = c->r[0] + (uint32_t)180;
    c->r[6] = c->r[0] + (uint32_t)6;
  L_8007FD94:;
    c->r[7] = (uint32_t)32769u << 16;
    c->r[31] = 0x8007FDA0u;
    c->r[7] = c->r[7] + (uint32_t)29444; func_80079374(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

static void leaf_80022D08(Core* c) {
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)382));
    c->r[2] = c->r[2] & 512u;
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)32780u << 16; if (_t) goto L_80022D9C; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-427));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)2; if (_t) goto L_80022D9C; }
    c->r[3] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)324));
    { int _t = (c->r[3] == c->r[2]);  if (_t) goto L_80022D9C; }
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)366));
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)32780u << 16; if (_t) goto L_80022D9C; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-1991));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_80022D9C; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)311));
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80022D9C; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)0));
    c->r[2] = c->r[2] & 2u;
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80022D9C; }
    { int _t = (c->r[7] != c->r[0]); c->r[2] = (uint32_t)32780u << 16; if (_t) goto L_80022DA4; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)0));
    c->r[2] = c->r[2] & 2u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)32780u << 16; if (_t) goto L_80022DA4; }
  L_80022D9C:;
    c->r[2] = c->r[0] + c->r[0]; return;
  L_80022DA4:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-427));
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80022D9C; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)0));
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)382));
    c->r[2] = c->r[2] | 2u;
    { int _t = ((int32_t)c->r[3] >= 0); c->mem_w8((c->r[4] + (uint32_t)0), (uint8_t)c->r[2]); if (_t) goto L_80022DCC; }
    c->r[6] = c->r[6] << 1;
  L_80022DCC:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)366));
    c->r[2] = c->r[2] - c->r[6];
    c->mem_w16((c->r[4] + (uint32_t)366), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    { int _t = ((int32_t)c->r[2] > 0); c->r[3] = (uint32_t)32780u << 16; if (_t) goto L_80022DF4; }
    c->r[2] = c->r[0] + (uint32_t)-1;
    c->mem_w16((c->r[4] + (uint32_t)366), (uint16_t)c->r[0]);
    c->mem_w8((c->r[3] + (uint32_t)-2035), (uint8_t)c->r[2]);
  L_80022DF4:;
    c->r[2] = (uint32_t)32780u << 16;
    c->mem_w8((c->r[2] + (uint32_t)-2018), (uint8_t)c->r[0]);
    c->r[2] = c->r[0] + (uint32_t)1; return;
    return;
}

static void leaf_80025744(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-56;
    c->mem_w32((c->r[29] + (uint32_t)48), c->r[20]);
    c->r[20] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)40), c->r[18]);
    c->r[18] = c->r[29] + (uint32_t)24;
    c->r[4] = c->r[18] + c->r[0];
    c->r[2] = c->r[0] + (uint32_t)8;
    c->mem_w8((c->r[29] + (uint32_t)17), (uint8_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)-32768;
    c->mem_w16((c->r[29] + (uint32_t)18), (uint16_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)32;
    c->mem_w16((c->r[29] + (uint32_t)24), (uint16_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)200;
    c->mem_w32((c->r[29] + (uint32_t)52), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)44), c->r[19]);
    c->mem_w32((c->r[29] + (uint32_t)36), c->r[17]);
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[16]);
    c->mem_w8((c->r[29] + (uint32_t)16), (uint8_t)c->r[0]);
    c->mem_w16((c->r[29] + (uint32_t)28), (uint16_t)c->r[0]);
    c->mem_w16((c->r[29] + (uint32_t)30), (uint16_t)c->r[0]);
    c->mem_w16((c->r[29] + (uint32_t)26), (uint16_t)c->r[2]);
    c->r[5] = c->mem_r32((c->r[20] + (uint32_t)56));
    c->r[6] = c->mem_r32((c->r[20] + (uint32_t)60));
    c->r[31] = 0x800257A8u;
    c->r[7] = c->r[29] + (uint32_t)16; func_8007E1B8(c);
    c->r[16] = (uint32_t)32769u << 16;
    c->r[16] = c->r[16] + (uint32_t)29492;
    c->r[4] = c->r[18] + c->r[0];
    c->r[2] = (uint32_t)32780u << 16;
    c->r[19] = c->r[2] + (uint32_t)-1936;
    c->r[5] = c->mem_r32((c->r[16] + (uint32_t)164));
    c->r[17] = c->r[29] + (uint32_t)16;
    c->mem_w16((c->r[29] + (uint32_t)18), (uint16_t)c->r[0]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[19] + (uint32_t)15));
    c->r[3] = (uint32_t)c->mem_r16((c->r[29] + (uint32_t)24));
    c->r[2] = c->r[2] + (uint32_t)-24;
    c->r[3] = c->r[3] + (uint32_t)24;
    c->mem_w16((c->r[29] + (uint32_t)28), (uint16_t)c->r[2]);
    c->mem_w16((c->r[29] + (uint32_t)24), (uint16_t)c->r[3]);
    c->r[6] = c->mem_r32((c->r[20] + (uint32_t)60));
    c->r[31] = 0x800257ECu;
    c->r[7] = c->r[17] + c->r[0]; func_8007E1B8(c);
    c->r[5] = c->mem_r32((c->r[16] + (uint32_t)168));
    c->r[2] = (uint32_t)c->mem_r16((c->r[29] + (uint32_t)24));
    c->r[3] = (uint32_t)c->mem_r16((c->r[29] + (uint32_t)28));
    c->r[4] = c->r[18] + c->r[0];
    c->mem_w16((c->r[29] + (uint32_t)28), (uint16_t)c->r[0]);
    c->r[2] = c->r[2] + c->r[3];
    c->mem_w16((c->r[29] + (uint32_t)24), (uint16_t)c->r[2]);
    c->r[6] = c->mem_r32((c->r[20] + (uint32_t)60));
    c->r[31] = 0x80025814u;
    c->r[7] = c->r[17] + c->r[0]; func_8007E1B8(c);
    c->r[2] = (uint32_t)c->mem_r8((c->r[19] + (uint32_t)14));
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)50; if (_t) goto L_80025914; }
    c->mem_w16((c->r[29] + (uint32_t)24), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)32782u << 16;
    c->r[3] = (uint32_t)c->mem_r8((c->r[19] + (uint32_t)14));
    c->r[4] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)32495));
    c->r[2] = c->r[0] + (uint32_t)204;
    c->mem_w16((c->r[29] + (uint32_t)26), (uint16_t)c->r[2]);
    c->mem_w16((c->r[29] + (uint32_t)28), (uint16_t)c->r[3]);
    c->r[3] = c->r[4] + (uint32_t)-18;
    c->r[2] = (uint32_t)(c->r[3] < (uint32_t)5);
    { int _t = (c->r[2] == c->r[0]); c->r[5] = c->r[0] + c->r[0]; if (_t) goto L_800258D4; }
    c->r[2] = (uint32_t)32769u << 16;
    c->r[2] = c->r[2] + (uint32_t)428;
    c->r[3] = c->r[3] << 2;
    c->r[3] = c->r[3] + c->r[2];
    c->r[2] = c->mem_r32((c->r[3] + (uint32_t)0));
    {  switch (c->r[2]) {  default: rec_dispatch(c, c->r[2]); return; } }
    return;
  L_800258D4:;
    { int _t = (c->r[5] == c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_800258FC; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)380));
    c->r[2] = c->r[2] & 3u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)32769u << 16; if (_t) goto L_800258FC; }
    c->r[5] = c->mem_r32((c->r[2] + (uint32_t)29632));
    c->r[4] = c->r[29] + (uint32_t)24; goto L_80025908;
  L_800258FC:;
    c->r[4] = c->r[29] + (uint32_t)24;
    c->r[2] = (uint32_t)32769u << 16;
    c->r[5] = c->mem_r32((c->r[2] + (uint32_t)29628));
  L_80025908:;
    c->r[6] = c->mem_r32((c->r[20] + (uint32_t)60));
    c->r[31] = 0x80025914u;
    c->r[7] = c->r[29] + (uint32_t)16; func_8007E1B8(c);
  L_80025914:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)52));
    c->r[20] = c->mem_r32((c->r[29] + (uint32_t)48));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)44));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)40));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)36));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[29] = c->r[29] + (uint32_t)56; return;
    return;
}

static void leaf_80025934(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-72;
    c->mem_w32((c->r[29] + (uint32_t)48), c->r[20]);
    c->r[20] = c->r[4] + c->r[0];
    c->r[2] = (uint32_t)32782u << 16;
    c->mem_w32((c->r[29] + (uint32_t)52), c->r[21]);
    c->r[21] = c->r[2] + (uint32_t)32384;
    c->mem_w32((c->r[29] + (uint32_t)64), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)60), c->r[23]);
    c->mem_w32((c->r[29] + (uint32_t)56), c->r[22]);
    c->mem_w32((c->r[29] + (uint32_t)44), c->r[19]);
    c->mem_w32((c->r[29] + (uint32_t)40), c->r[18]);
    c->mem_w32((c->r[29] + (uint32_t)36), c->r[17]);
    { int _t = (c->r[5] != c->r[0]); c->mem_w32((c->r[29] + (uint32_t)32), c->r[16]); if (_t) goto L_8002597C; }
    c->r[19] = c->r[0] + c->r[0];
    c->r[18] = c->r[19] + c->r[0];
    c->mem_w8((c->r[29] + (uint32_t)17), (uint8_t)c->r[0]); goto L_80025988;
  L_8002597C:;
    c->r[18] = c->r[0] + (uint32_t)16;
    c->r[19] = c->r[0] + (uint32_t)40;
    c->mem_w8((c->r[29] + (uint32_t)17), (uint8_t)c->r[6]);
  L_80025988:;
    c->r[2] = (uint32_t)32778u << 16;
    c->mem_w16((c->r[29] + (uint32_t)18), (uint16_t)c->r[0]);
    c->mem_w16((c->r[29] + (uint32_t)28), (uint16_t)c->r[0]);
    c->mem_w16((c->r[29] + (uint32_t)30), (uint16_t)c->r[0]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[20] + (uint32_t)10));
    c->r[2] = c->r[2] + (uint32_t)-11508;
    c->r[3] = c->r[3] << 2;
    c->r[3] = c->r[3] + c->r[2];
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[21] + (uint32_t)366));
    c->r[17] = c->mem_r32((c->r[3] + (uint32_t)0));
    { int _t = ((int32_t)c->r[2] <= 0); c->r[16] = c->r[0] + c->r[0]; if (_t) goto L_80025A24; }
    c->r[2] = (uint32_t)32769u << 16;
    c->r[22] = c->r[2] + (uint32_t)29492;
    c->r[2] = c->r[16] << 2;
  L_800259C4:;
    c->r[3] = c->r[17] + c->r[2];
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)3));
    c->mem_w8((c->r[29] + (uint32_t)16), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)1));
    c->r[2] = c->r[2] + c->r[18];
    c->mem_w16((c->r[29] + (uint32_t)24), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)2));
    c->r[4] = c->r[29] + (uint32_t)24;
    c->r[2] = c->r[2] + c->r[19];
    c->mem_w16((c->r[29] + (uint32_t)26), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)0));
    c->r[6] = c->mem_r32((c->r[20] + (uint32_t)60));
    c->r[2] = c->r[2] << 2;
    c->r[2] = c->r[2] + c->r[22];
    c->r[5] = c->mem_r32((c->r[2] + (uint32_t)0));
    c->r[31] = 0x80025A10u;
    c->r[7] = c->r[29] + (uint32_t)16; func_8007E1B8(c);
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[21] + (uint32_t)366));
    c->r[16] = c->r[16] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[16] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[16] << 2; if (_t) goto L_800259C4; }
  L_80025A24:;
    c->r[2] = (uint32_t)32780u << 16;
    c->r[3] = c->r[2] + (uint32_t)-1936;
    c->r[16] = (uint32_t)(int16_t)c->mem_r16((c->r[21] + (uint32_t)366));
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)13));
    c->r[2] = (uint32_t)((int32_t)c->r[16] < (int32_t)c->r[2]);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)32769u << 16; if (_t) goto L_80025AB4; }
    c->r[23] = c->r[2] + (uint32_t)29492;
    c->r[22] = c->r[3] + c->r[0];
    c->r[2] = c->r[16] << 2;
  L_80025A50:;
    c->r[3] = c->r[17] + c->r[2];
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)3));
    c->mem_w8((c->r[29] + (uint32_t)16), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)1));
    c->r[2] = c->r[2] + c->r[18];
    c->mem_w16((c->r[29] + (uint32_t)24), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)2));
    c->r[4] = c->r[29] + (uint32_t)24;
    c->r[2] = c->r[2] + c->r[19];
    c->mem_w16((c->r[29] + (uint32_t)26), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)0));
    c->r[6] = c->mem_r32((c->r[20] + (uint32_t)60));
    c->r[2] = c->r[2] + (uint32_t)1;
    c->r[2] = c->r[2] << 2;
    c->r[2] = c->r[2] + c->r[23];
    c->r[5] = c->mem_r32((c->r[2] + (uint32_t)0));
    c->r[31] = 0x80025AA0u;
    c->r[7] = c->r[29] + (uint32_t)16; func_8007E1B8(c);
    c->r[2] = (uint32_t)c->mem_r8((c->r[22] + (uint32_t)13));
    c->r[16] = c->r[16] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[16] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[16] << 2; if (_t) goto L_80025A50; }
  L_80025AB4:;
    c->r[4] = c->r[29] + (uint32_t)24;
    c->r[7] = c->r[29] + (uint32_t)16;
    c->r[16] = c->r[0] + c->r[0];
    c->r[22] = c->r[0] + (uint32_t)4;
    c->r[2] = c->r[18] + (uint32_t)32;
    c->mem_w16((c->r[29] + (uint32_t)24), (uint16_t)c->r[2]);
    c->r[2] = c->r[19] + (uint32_t)32;
    c->r[3] = (uint32_t)32769u << 16;
    c->r[3] = c->r[3] + (uint32_t)29492;
    c->mem_w8((c->r[29] + (uint32_t)16), (uint8_t)c->r[0]);
    c->mem_w16((c->r[29] + (uint32_t)18), (uint16_t)c->r[0]);
    c->mem_w16((c->r[29] + (uint32_t)28), (uint16_t)c->r[0]);
    c->mem_w16((c->r[29] + (uint32_t)30), (uint16_t)c->r[0]);
    c->mem_w16((c->r[29] + (uint32_t)26), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[21] + (uint32_t)366));
    c->r[6] = c->mem_r32((c->r[20] + (uint32_t)60));
    c->r[2] = c->r[2] + (uint32_t)17;
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 14);
    c->r[2] = c->r[2] + c->r[3];
    c->r[5] = c->mem_r32((c->r[2] + (uint32_t)0));
    c->r[31] = 0x80025B10u;
    c->r[17] = c->r[3] + c->r[0]; func_8007E6DC(c);
    c->r[2] = c->r[0] + (uint32_t)-32768;
    c->mem_w16((c->r[29] + (uint32_t)18), (uint16_t)c->r[2]);
    c->r[4] = c->r[29] + (uint32_t)24;
  L_80025B1C:;
    c->r[7] = c->r[29] + (uint32_t)16;
    c->r[2] = c->r[22] - c->r[16];
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 14);
    c->r[2] = c->r[2] + c->r[17];
    c->r[5] = c->mem_r32((c->r[2] + (uint32_t)0));
    c->r[6] = c->mem_r32((c->r[20] + (uint32_t)60));
    c->r[31] = 0x80025B40u;
    c->r[16] = c->r[16] + (uint32_t)1; func_8007E1B8(c);
    c->r[2] = (uint32_t)((int32_t)c->r[16] < 2);
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[29] + (uint32_t)24; if (_t) goto L_80025B1C; }
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)64));
    c->r[23] = c->mem_r32((c->r[29] + (uint32_t)60));
    c->r[22] = c->mem_r32((c->r[29] + (uint32_t)56));
    c->r[21] = c->mem_r32((c->r[29] + (uint32_t)52));
    c->r[20] = c->mem_r32((c->r[29] + (uint32_t)48));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)44));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)40));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)36));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[29] = c->r[29] + (uint32_t)72; return;
    return;
}

static void leaf_80025B78(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-72;
    c->mem_w32((c->r[29] + (uint32_t)44), c->r[17]);
    c->r[17] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)52), c->r[19]);
    c->r[19] = c->r[29] + (uint32_t)32;
    c->mem_w32((c->r[29] + (uint32_t)48), c->r[18]);
    c->r[18] = (uint32_t)32780u << 16;
    c->mem_w32((c->r[29] + (uint32_t)40), c->r[16]);
    c->r[16] = c->mem_r32((c->r[18] + (uint32_t)-2748));
    c->r[2] = c->r[0] + (uint32_t)320;
    c->mem_w16((c->r[29] + (uint32_t)36), (uint16_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)240;
    c->mem_w32((c->r[29] + (uint32_t)56), c->r[20]);
    c->r[20] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[29] + (uint32_t)38), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[20] + (uint32_t)309));
    c->r[5] = c->r[19] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)64), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)60), c->r[21]);
    c->mem_w16((c->r[29] + (uint32_t)32), (uint16_t)c->r[0]);
    c->r[4] = c->r[16] + c->r[0];
    c->r[2] = c->r[2] << 8;
    c->r[31] = 0x80025BD8u;
    c->mem_w16((c->r[29] + (uint32_t)34), (uint16_t)c->r[2]); func_80081CF8(c);
    c->r[21] = (uint32_t)32783u << 16;
    c->r[4] = c->mem_r32((c->r[21] + (uint32_t)-10040));
    c->r[5] = c->r[16] + c->r[0];
    c->r[31] = 0x80025BECu;
    c->r[4] = c->r[4] + (uint32_t)32; func_80083C30(c);
    c->r[3] = c->mem_r32((c->r[18] + (uint32_t)-2748));
    c->r[2] = c->r[0] + (uint32_t)8;
    c->mem_w8((c->r[29] + (uint32_t)25), (uint8_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)212;
    c->mem_w8((c->r[29] + (uint32_t)24), (uint8_t)c->r[0]);
    c->mem_w16((c->r[29] + (uint32_t)26), (uint16_t)c->r[0]);
    c->mem_w16((c->r[29] + (uint32_t)36), (uint16_t)c->r[0]);
    c->mem_w16((c->r[29] + (uint32_t)38), (uint16_t)c->r[0]);
    c->r[3] = c->r[3] + (uint32_t)12;
    c->mem_w32((c->r[18] + (uint32_t)-2748), c->r[3]);
    c->r[5] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)6));
    c->r[6] = c->r[0] + (uint32_t)212;
    c->mem_w16((c->r[29] + (uint32_t)34), (uint16_t)c->r[2]);
    c->r[5] = c->r[5] << 24;
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 24);
    c->r[5] = c->r[5] + (uint32_t)160;
    c->mem_w16((c->r[29] + (uint32_t)32), (uint16_t)c->r[5]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)8));
    c->r[7] = c->r[0] + (uint32_t)8;
    c->r[2] = c->r[17] + c->r[2];
    c->r[4] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)34));
    c->r[2] = (uint32_t)32778u << 16;
    c->r[16] = c->r[2] + (uint32_t)-11644;
    c->r[2] = c->r[4] << 2;
    c->r[2] = c->r[2] + c->r[16];
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)2));
    c->r[5] = c->r[5] << 16;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[2]);
    c->r[4] = c->mem_r32((c->r[17] + (uint32_t)60));
    c->r[31] = 0x80025C68u;
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16); func_8007E938(c);
    c->r[2] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)8));
    c->r[4] = c->r[2] + (uint32_t)-1;
    { int _t = ((int32_t)c->r[4] >= 0); c->r[2] = c->r[17] + c->r[4]; if (_t) goto L_80025C8C; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)7));
    c->r[4] = c->r[2] + (uint32_t)-1;
    c->r[2] = c->r[17] + c->r[4];
  L_80025C8C:;
    c->r[3] = (uint32_t)c->mem_r16((c->r[29] + (uint32_t)32));
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)34));
    c->r[6] = (uint32_t)(int16_t)c->mem_r16((c->r[29] + (uint32_t)34));
    c->r[7] = (uint32_t)c->mem_r8((c->r[29] + (uint32_t)25));
    c->r[3] = c->r[3] + (uint32_t)-32;
    c->r[2] = c->r[2] << 2;
    c->r[2] = c->r[2] + c->r[16];
    c->mem_w16((c->r[29] + (uint32_t)32), (uint16_t)c->r[3]);
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)2));
    c->r[5] = c->r[3] << 16;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[2]);
    c->r[4] = c->mem_r32((c->r[17] + (uint32_t)60));
    c->r[31] = 0x80025CC4u;
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16); func_8007E938(c);
    c->r[3] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)8));
    c->r[2] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)7));
    c->r[4] = c->r[3] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[4] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[17] + c->r[4]; if (_t) goto L_80025CE4; }
    c->r[4] = c->r[0] + c->r[0];
    c->r[2] = c->r[17] + c->r[4];
  L_80025CE4:;
    c->r[3] = (uint32_t)c->mem_r16((c->r[29] + (uint32_t)32));
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)34));
    c->r[6] = (uint32_t)(int16_t)c->mem_r16((c->r[29] + (uint32_t)34));
    c->r[7] = (uint32_t)c->mem_r8((c->r[29] + (uint32_t)25));
    c->r[3] = c->r[3] + (uint32_t)64;
    c->r[2] = c->r[2] << 2;
    c->r[2] = c->r[2] + c->r[16];
    c->mem_w16((c->r[29] + (uint32_t)32), (uint16_t)c->r[3]);
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)2));
    c->r[5] = c->r[3] << 16;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[2]);
    c->r[4] = c->mem_r32((c->r[17] + (uint32_t)60));
    c->r[31] = 0x80025D1Cu;
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16); func_8007E938(c);
    c->r[5] = c->r[19] + c->r[0];
    c->r[16] = c->mem_r32((c->r[18] + (uint32_t)-2748));
    c->r[2] = c->r[0] + (uint32_t)144;
    c->mem_w16((c->r[29] + (uint32_t)32), (uint16_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)32;
    c->mem_w16((c->r[29] + (uint32_t)36), (uint16_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)224;
    c->mem_w16((c->r[29] + (uint32_t)38), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[20] + (uint32_t)309));
    c->r[4] = c->r[16] + c->r[0];
    c->r[2] = c->r[2] << 8;
    c->r[31] = 0x80025D50u;
    c->mem_w16((c->r[29] + (uint32_t)34), (uint16_t)c->r[2]); func_80081CF8(c);
    c->r[5] = c->r[16] + c->r[0];
    c->r[4] = (uint32_t)c->mem_r8((c->r[29] + (uint32_t)25));
    c->r[2] = c->mem_r32((c->r[21] + (uint32_t)-10040));
    c->r[4] = c->r[4] << 2;
    c->r[31] = 0x80025D68u;
    c->r[4] = c->r[2] + c->r[4]; func_80083C30(c);
    c->r[2] = c->mem_r32((c->r[18] + (uint32_t)-2748));
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)64));
    c->r[21] = c->mem_r32((c->r[29] + (uint32_t)60));
    c->r[20] = c->mem_r32((c->r[29] + (uint32_t)56));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)52));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)44));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)40));
    c->r[2] = c->r[2] + (uint32_t)12;
    c->mem_w32((c->r[18] + (uint32_t)-2748), c->r[2]);
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)48));
    c->r[29] = c->r[29] + (uint32_t)72; return;
    return;
}

static void leaf_80027768(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-40;
    c->r[11] = c->r[4] + c->r[0];
    c->r[2] = (uint32_t)32780u << 16;
    c->r[10] = c->mem_r32((c->r[2] + (uint32_t)-2748));
    c->r[2] = (uint32_t)8064u << 16;
    c->r[15] = c->r[2] + (uint32_t)144;
    c->r[5] = c->r[5] << 22;
    c->r[14] = c->r[29] + (uint32_t)32;
    c->r[6] = c->r[6] << 16;
    c->r[6] = (uint32_t)((int32_t)c->r[6] >> 16);
    c->r[9] = c->r[11] + (uint32_t)24;
    c->r[8] = c->r[10] + (uint32_t)40;
  L_80027798:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[9] + (uint32_t)4));
    c->r[13] = c->mem_r32((c->r[9] + (uint32_t)-20));
    c->r[2] = c->r[2] << 8;
    c->mem_w16((c->r[29] + (uint32_t)0), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[9] + (uint32_t)6));
    c->r[2] = c->r[2] << 24;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->mem_w16((c->r[29] + (uint32_t)2), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[9] + (uint32_t)-9));
    c->r[2] = c->r[2] << 24;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->mem_w16((c->r[29] + (uint32_t)4), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[9] + (uint32_t)5));
    c->r[2] = c->r[2] << 8;
    c->mem_w16((c->r[29] + (uint32_t)8), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[9] + (uint32_t)7));
    c->r[2] = c->r[2] << 8;
    c->mem_w16((c->r[29] + (uint32_t)10), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[9] + (uint32_t)-5));
    c->r[2] = c->r[2] << 8;
    c->mem_w16((c->r[29] + (uint32_t)12), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[9] + (uint32_t)8));
    c->r[2] = c->r[2] << 8;
    c->mem_w16((c->r[29] + (uint32_t)16), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[9] + (uint32_t)10));
    c->r[2] = c->r[2] << 8;
    c->mem_w16((c->r[29] + (uint32_t)18), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[9] + (uint32_t)-1));
    c->r[3] = c->r[29] + (uint32_t)8;
    c->r[2] = c->r[2] << 8;
    c->mem_w16((c->r[29] + (uint32_t)20), (uint16_t)c->r[2]);
    c->r[2] = c->r[29] + (uint32_t)16;
    gte_write_data(0, c->mem_r32((c->r[29] + (uint32_t)0)));
    gte_write_data(1, c->mem_r32((c->r[29] + (uint32_t)4)));
    gte_write_data(2, c->mem_r32((c->r[3] + (uint32_t)0)));
    gte_write_data(3, c->mem_r32((c->r[3] + (uint32_t)4)));
    gte_write_data(4, c->mem_r32((c->r[2] + (uint32_t)0)));
    gte_write_data(5, c->mem_r32((c->r[2] + (uint32_t)4)));
    c->r[2] = (uint32_t)c->mem_r8((c->r[9] + (uint32_t)9));
    c->r[2] = c->r[2] << 8;
    c->mem_w16((c->r[29] + (uint32_t)24), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[9] + (uint32_t)11));
    c->r[2] = c->r[2] << 8;
    c->mem_w16((c->r[29] + (uint32_t)26), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[9] + (uint32_t)3));
    c->r[2] = c->r[2] << 8;
    c->mem_w16((c->r[29] + (uint32_t)28), (uint16_t)c->r[2]);
    gte_op(c, 0x4A280030u);
    c->r[12] = gte_read_ctrl(31);
    c->mem_w32((c->r[14] + (uint32_t)0), c->r[12]);
    c->r[2] = c->mem_r32((c->r[29] + (uint32_t)32));
    { int _t = ((int32_t)c->r[2] < 0);  if (_t) goto L_80027A30; }
    c->mem_w32((c->r[10] + (uint32_t)8), gte_read_data(12));
    c->mem_w32((c->r[10] + (uint32_t)20), gte_read_data(13));
    c->mem_w32((c->r[10] + (uint32_t)32), gte_read_data(14));
    c->r[2] = c->r[29] + (uint32_t)24;
    gte_write_data(0, c->mem_r32((c->r[2] + (uint32_t)0)));
    gte_write_data(1, c->mem_r32((c->r[2] + (uint32_t)4)));
    c->r[2] = c->mem_r32((c->r[11] + (uint32_t)0));
    c->r[2] = c->r[2] + c->r[5];
    c->mem_w32((c->r[8] + (uint32_t)-28), c->r[2]);
    gte_op(c, 0x4A180001u);
    c->r[12] = gte_read_ctrl(31);
    c->mem_w32((c->r[14] + (uint32_t)0), c->r[12]);
    c->r[2] = c->mem_r32((c->r[29] + (uint32_t)32));
    { int _t = ((int32_t)c->r[2] < 0); c->r[2] = c->r[10] + (uint32_t)44; if (_t) goto L_80027A30; }
    c->mem_w32((c->r[2] + (uint32_t)0), gte_read_data(14));
    c->r[2] = (uint32_t)127u << 16;
    c->r[2] = c->r[2] | 65535u;
    c->r[2] = c->r[13] & c->r[2];
    c->mem_w32((c->r[8] + (uint32_t)-16), c->r[2]);
    gte_op(c, 0x4B68002Eu);
    c->r[2] = c->r[29] + (uint32_t)36;
    c->mem_w32((c->r[2] + (uint32_t)0), gte_read_data(7));
    c->r[2] = c->mem_r32((c->r[29] + (uint32_t)36));
    c->r[2] = c->r[2] + c->r[6];
    c->r[3] = (uint32_t)((int32_t)c->r[2] >> 10);
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> (c->r[3] & 31));
    c->r[3] = c->r[3] << 9;
    c->r[2] = c->r[2] + c->r[3];
    c->mem_w32((c->r[29] + (uint32_t)36), c->r[2]);
    c->r[2] = c->r[2] + (uint32_t)-4;
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)2044);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)-1; if (_t) goto L_80027938; }
    c->mem_w32((c->r[29] + (uint32_t)36), c->r[2]);
  L_80027938:;
    c->r[2] = c->mem_r32((c->r[29] + (uint32_t)36));
    { int _t = ((int32_t)c->r[2] < 0);  if (_t) goto L_80027A30; }
    c->r[2] = c->mem_r32((c->r[9] + (uint32_t)-16));
    c->r[3] = (uint32_t)c->mem_r8((c->r[8] + (uint32_t)-16));
    c->mem_w32((c->r[8] + (uint32_t)-4), c->r[2]);
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->mem_w32((c->r[8] + (uint32_t)8), c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[8] + (uint32_t)-28));
    c->r[3] = c->r[3] + c->r[7];
    c->mem_w8((c->r[8] + (uint32_t)-16), (uint8_t)c->r[3]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[8] + (uint32_t)8));
    c->r[2] = c->r[2] + c->r[7];
    c->mem_w8((c->r[8] + (uint32_t)-28), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[8] + (uint32_t)-4));
    c->r[3] = c->r[3] + c->r[7];
    c->mem_w8((c->r[8] + (uint32_t)8), (uint8_t)c->r[3]);
    c->r[2] = c->r[2] + c->r[7];
    c->mem_w8((c->r[8] + (uint32_t)-4), (uint8_t)c->r[2]);
    c->r[24] = c->mem_r32((c->r[15] + (uint32_t)0));
    gte_write_data(8, c->r[24]);
    c->r[4] = c->r[11] + (uint32_t)12;
    c->r[3] = c->r[11] + (uint32_t)16;
    c->r[2] = c->r[11] + (uint32_t)20;
    gte_write_data(20, c->mem_r32((c->r[4] + (uint32_t)0)));
    gte_write_data(21, c->mem_r32((c->r[3] + (uint32_t)0)));
    gte_write_data(22, c->mem_r32((c->r[2] + (uint32_t)0)));
    gte_write_data(6, c->mem_r32((c->r[2] + (uint32_t)0)));
    gte_op(c, 0x4AF8002Au);
    c->r[4] = c->r[10] + (uint32_t)4;
    c->r[3] = c->r[10] + (uint32_t)16;
    c->r[2] = c->r[10] + (uint32_t)28;
    c->mem_w32((c->r[4] + (uint32_t)0), gte_read_data(20));
    c->mem_w32((c->r[3] + (uint32_t)0), gte_read_data(21));
    c->mem_w32((c->r[2] + (uint32_t)0), gte_read_data(22));
    c->r[2] = (uint32_t)16384u << 16;
    c->r[2] = c->r[13] & c->r[2];
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)62; if (_t) goto L_800279E8; }
    c->r[2] = c->r[0] + (uint32_t)60;
  L_800279E8:;
    c->mem_w8((c->r[8] + (uint32_t)-33), (uint8_t)c->r[2]);
    gte_write_data(6, c->mem_r32((c->r[9] + (uint32_t)0)));
    gte_op(c, 0x4A780010u);
    c->mem_w32((c->r[8] + (uint32_t)0), gte_read_data(22));
    c->r[3] = (uint32_t)32783u << 16;
    c->r[2] = c->mem_r32((c->r[29] + (uint32_t)36));
    c->r[4] = c->mem_r32((c->r[3] + (uint32_t)-10040));
    c->r[2] = c->r[2] << 2;
    c->r[4] = c->r[4] + c->r[2];
    c->r[2] = c->mem_r32((c->r[4] + (uint32_t)0));
    c->r[3] = (uint32_t)3072u << 16;
    c->r[2] = c->r[2] | c->r[3];
    c->mem_w32((c->r[10] + (uint32_t)0), c->r[2]);
    c->mem_w32((c->r[4] + (uint32_t)0), c->r[10]);
    c->r[8] = c->r[8] + (uint32_t)52;
    c->r[10] = c->r[10] + (uint32_t)52;
  L_80027A30:;
    c->r[9] = c->r[9] + (uint32_t)36;
    { int _t = ((int32_t)c->r[13] > 0); c->r[11] = c->r[11] + (uint32_t)36; if (_t) goto L_80027798; }
    c->r[2] = (uint32_t)32780u << 16;
    c->mem_w32((c->r[2] + (uint32_t)-2748), c->r[10]);
    c->r[29] = c->r[29] + (uint32_t)40; return;
    return;
}

static void leaf_8003A1E4(Core* c) {
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)8));
    c->r[3] = c->r[2] >> 1;
    c->r[2] = c->r[2] & 1u;
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_8003A248; }
    c->r[5] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)63));
    { int _t = (c->r[5] != c->r[3]); c->r[2] = (uint32_t)((int32_t)c->r[5] < (int32_t)c->r[3]); if (_t) goto L_8003A214; }
    c->r[2] = c->r[0] + c->r[0]; return;
  L_8003A214:;
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_8003A234; }
    c->r[3] = c->r[3] - c->r[5];
    c->r[2] = c->r[3] << 1;
    c->r[2] = c->r[2] + c->r[3];
    c->r[2] = c->r[2] << 2;
    c->r[2] = c->r[0] - c->r[2]; return;
  L_8003A234:;
    c->r[3] = c->r[5] - c->r[3];
    c->r[2] = c->r[3] << 1;
    c->r[2] = c->r[2] + c->r[3];
    c->r[2] = c->r[2] << 2; return;
  L_8003A248:;
    c->r[5] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)63));
    c->r[2] = (uint32_t)((int32_t)c->r[5] < (int32_t)c->r[3]);
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_8003A278; }
    c->r[3] = c->r[3] - c->r[5];
    c->r[2] = c->r[3] << 1;
    c->r[2] = c->r[2] + c->r[3];
    c->r[2] = c->r[2] << 2;
    c->r[2] = c->r[0] - c->r[2];
    c->r[2] = c->r[2] + (uint32_t)6; return;
  L_8003A278:;
    c->r[3] = c->r[5] - c->r[3];
    c->r[2] = c->r[3] << 1;
    c->r[2] = c->r[2] + c->r[3];
    c->r[2] = c->r[2] << 2;
    c->r[2] = c->r[2] + (uint32_t)6; return;
    return;
}

static void leaf_8003D23C(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-40;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
    c->r[18] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[19]);
    c->r[19] = (uint32_t)8064u << 16;
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[17] = (uint32_t)8064u << 16;
    c->r[17] = c->r[17] + (uint32_t)0;
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[18] + (uint32_t)12));
    c->r[4] = c->r[17] + c->r[0];
    c->r[5] = c->r[2] << 2;
    c->r[5] = c->r[5] + c->r[2];
    c->r[5] = c->r[5] << 16;
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16);
    c->r[6] = c->r[5] + c->r[0];
    c->r[31] = 0x8003D288u;
    c->r[7] = c->r[5] + c->r[0]; func_800517BC(c);
    c->r[16] = (uint32_t)8064u << 16;
    c->r[16] = c->r[16] + (uint32_t)32;
    c->r[31] = 0x8003D298u;
    c->r[4] = c->r[16] + c->r[0]; func_80051794(c);
    c->r[4] = c->r[18] + (uint32_t)40;
    c->r[31] = 0x8003D2A4u;
    c->r[5] = c->r[16] + c->r[0]; func_800847F0(c);
    c->r[4] = c->r[16] + c->r[0];
    c->r[5] = c->r[17] + c->r[0];
    c->r[16] = (uint32_t)8064u << 16;
    c->r[16] = c->r[16] + (uint32_t)64;
    c->r[31] = 0x8003D2BCu;
    c->r[6] = c->r[16] + c->r[0]; func_80084110(c);
    c->r[2] = (uint32_t)8064u << 16;
    c->r[19] = c->r[19] | 32u;
    c->r[17] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)c->mem_r16((c->r[18] + (uint32_t)30));
    c->r[8] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[2] + (uint32_t)192), (uint16_t)c->r[3]);
    c->r[3] = (uint32_t)c->mem_r16((c->r[18] + (uint32_t)34));
    c->r[2] = c->r[2] + (uint32_t)192;
    c->mem_w16((c->r[2] + (uint32_t)2), (uint16_t)c->r[3]);
    c->r[3] = (uint32_t)c->mem_r16((c->r[18] + (uint32_t)38));
    c->r[8] = c->r[8] + (uint32_t)248;
    c->mem_w16((c->r[2] + (uint32_t)4), (uint16_t)c->r[3]);
    c->r[12] = c->mem_r32((c->r[8] + (uint32_t)0));
    c->r[13] = c->mem_r32((c->r[8] + (uint32_t)4));
    gte_write_ctrl(0, c->r[12]);
    gte_write_ctrl(1, c->r[13]);
    c->r[12] = c->mem_r32((c->r[8] + (uint32_t)8));
    c->r[13] = c->mem_r32((c->r[8] + (uint32_t)12));
    c->r[14] = c->mem_r32((c->r[8] + (uint32_t)16));
    gte_write_ctrl(2, c->r[12]);
    gte_write_ctrl(3, c->r[13]);
    gte_write_ctrl(4, c->r[14]);
    gte_write_data(0, c->mem_r32((c->r[2] + (uint32_t)0)));
    gte_write_data(1, c->mem_r32((c->r[2] + (uint32_t)4)));
    gte_op(c, 0x4A486012u);
    c->r[8] = (uint32_t)8064u << 16;
    c->r[8] = c->r[8] + (uint32_t)84;
    c->mem_w32((c->r[8] + (uint32_t)0), gte_read_data(25));
    c->mem_w32((c->r[8] + (uint32_t)4), gte_read_data(26));
    c->mem_w32((c->r[8] + (uint32_t)8), gte_read_data(27));
    c->r[4] = (uint32_t)8064u << 16;
    c->r[4] = c->r[4] + (uint32_t)208;
    c->r[2] = c->mem_r32((c->r[16] + (uint32_t)20));
    c->r[3] = c->mem_r32((c->r[4] + (uint32_t)60));
    c->r[2] = c->r[2] + c->r[3];
    c->mem_w32((c->r[16] + (uint32_t)20), c->r[2]);
    c->r[2] = c->mem_r32((c->r[16] + (uint32_t)24));
    c->r[5] = c->mem_r32((c->r[4] + (uint32_t)64));
    c->r[3] = c->mem_r32((c->r[16] + (uint32_t)28));
    c->r[4] = c->mem_r32((c->r[4] + (uint32_t)68));
    c->r[2] = c->r[2] + c->r[5];
    c->r[3] = c->r[3] + c->r[4];
    c->mem_w32((c->r[16] + (uint32_t)24), c->r[2]);
    c->mem_w32((c->r[16] + (uint32_t)28), c->r[3]);
    c->r[12] = c->mem_r32((c->r[16] + (uint32_t)0));
    c->r[13] = c->mem_r32((c->r[16] + (uint32_t)4));
    gte_write_ctrl(0, c->r[12]);
    gte_write_ctrl(1, c->r[13]);
    c->r[12] = c->mem_r32((c->r[16] + (uint32_t)8));
    c->r[13] = c->mem_r32((c->r[16] + (uint32_t)12));
    c->r[14] = c->mem_r32((c->r[16] + (uint32_t)16));
    gte_write_ctrl(2, c->r[12]);
    gte_write_ctrl(3, c->r[13]);
    gte_write_ctrl(4, c->r[14]);
    c->r[12] = c->mem_r32((c->r[16] + (uint32_t)20));
    c->r[13] = c->mem_r32((c->r[16] + (uint32_t)24));
    gte_write_ctrl(5, c->r[12]);
    c->r[14] = c->mem_r32((c->r[16] + (uint32_t)28));
    gte_write_ctrl(6, c->r[13]);
    gte_write_ctrl(7, c->r[14]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)7));
    c->r[2] = c->r[3] << 1;
    c->r[2] = c->r[2] + c->r[3];
    c->r[2] = c->r[2] << 3;
    c->r[3] = (uint32_t)32778u << 16;
    c->r[3] = c->r[3] + (uint32_t)-11156;
    c->r[2] = c->r[2] + c->r[3];
    gte_write_data(0, c->mem_r32((c->r[2] + (uint32_t)0)));
    gte_write_data(1, c->mem_r32((c->r[2] + (uint32_t)4)));
    gte_write_data(2, c->mem_r32((c->r[2] + (uint32_t)8)));
    gte_write_data(3, c->mem_r32((c->r[2] + (uint32_t)12)));
    gte_write_data(4, c->mem_r32((c->r[2] + (uint32_t)16)));
    gte_write_data(5, c->mem_r32((c->r[2] + (uint32_t)20)));
    gte_op(c, 0x4A280030u);
    c->mem_w32((c->r[17] + (uint32_t)8), gte_read_data(12));
    c->mem_w32((c->r[17] + (uint32_t)16), gte_read_data(13));
    c->mem_w32((c->r[17] + (uint32_t)24), gte_read_data(14));
    gte_op(c, 0x4B58002Du);
    c->mem_w32((c->r[19] + (uint32_t)0), gte_read_data(7));
    c->r[2] = c->mem_r32((c->r[19] + (uint32_t)0));
    c->r[3] = (uint32_t)((int32_t)c->r[2] >> 10);
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> (c->r[3] & 31));
    c->r[3] = c->r[3] << 9;
    c->r[2] = c->r[2] + c->r[3];
    c->mem_w32((c->r[19] + (uint32_t)0), c->r[2]);
    c->r[2] = c->r[2] + (uint32_t)-4;
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)2044);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)-1; if (_t) goto L_8003D448; }
    c->mem_w32((c->r[19] + (uint32_t)0), c->r[2]);
  L_8003D448:;
    c->r[6] = c->mem_r32((c->r[19] + (uint32_t)0));
    { int _t = ((int32_t)c->r[6] < 0); c->r[4] = (uint32_t)32780u << 16; if (_t) goto L_8003D568; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)14));
    c->r[5] = c->mem_r32((c->r[4] + (uint32_t)-2748));
    c->mem_w8((c->r[17] + (uint32_t)7), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)52));
    c->mem_w8((c->r[17] + (uint32_t)12), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)53));
    c->mem_w8((c->r[17] + (uint32_t)13), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)56));
    c->mem_w8((c->r[17] + (uint32_t)20), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)57));
    c->mem_w8((c->r[17] + (uint32_t)21), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)60));
    c->mem_w8((c->r[17] + (uint32_t)28), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)61));
    c->mem_w8((c->r[17] + (uint32_t)29), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[18] + (uint32_t)58));
    c->mem_w16((c->r[17] + (uint32_t)22), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[18] + (uint32_t)54));
    c->mem_w16((c->r[17] + (uint32_t)14), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)48));
    c->mem_w8((c->r[17] + (uint32_t)4), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)49));
    c->mem_w8((c->r[17] + (uint32_t)5), (uint8_t)c->r[2]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)50));
    c->r[2] = c->r[5] + (uint32_t)32;
    c->mem_w32((c->r[4] + (uint32_t)-2748), c->r[2]);
    c->mem_w8((c->r[17] + (uint32_t)6), (uint8_t)c->r[3]);
    c->r[2] = (uint32_t)32783u << 16;
    c->r[4] = c->mem_r32((c->r[2] + (uint32_t)-10040));
    c->r[2] = c->r[6] << 2;
    c->r[4] = c->r[4] + c->r[2];
    c->r[2] = c->mem_r32((c->r[4] + (uint32_t)0));
    c->r[3] = (uint32_t)1792u << 16;
    c->r[2] = c->r[2] | c->r[3];
    c->mem_w32((c->r[5] + (uint32_t)0), c->r[2]);
    c->mem_w32((c->r[4] + (uint32_t)0), c->r[5]);
    c->r[5] = c->r[5] + (uint32_t)4;
    c->r[2] = c->mem_r32((c->r[17] + (uint32_t)4));
    c->mem_w32((c->r[5] + (uint32_t)0), c->r[2]);
    c->r[2] = c->mem_r32((c->r[17] + (uint32_t)8));
    c->r[5] = c->r[5] + (uint32_t)4;
    c->mem_w32((c->r[5] + (uint32_t)0), c->r[2]);
    c->r[2] = c->mem_r32((c->r[17] + (uint32_t)12));
    c->r[5] = c->r[5] + (uint32_t)4;
    c->mem_w32((c->r[5] + (uint32_t)0), c->r[2]);
    c->r[2] = c->mem_r32((c->r[17] + (uint32_t)16));
    c->r[5] = c->r[5] + (uint32_t)4;
    c->mem_w32((c->r[5] + (uint32_t)0), c->r[2]);
    c->r[2] = c->mem_r32((c->r[17] + (uint32_t)20));
    c->r[5] = c->r[5] + (uint32_t)4;
    c->mem_w32((c->r[5] + (uint32_t)0), c->r[2]);
    c->r[2] = c->mem_r32((c->r[17] + (uint32_t)24));
    c->r[5] = c->r[5] + (uint32_t)4;
    c->mem_w32((c->r[5] + (uint32_t)0), c->r[2]);
    c->r[2] = c->mem_r32((c->r[17] + (uint32_t)28));
    c->mem_w32((c->r[5] + (uint32_t)4), c->r[2]);
  L_8003D568:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)40; return;
    return;
}

static void leaf_800455C0(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-40;
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[19]);
    c->r[19] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[17] = c->r[5] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
    c->r[18] = c->r[6] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[7] << 16;
    c->r[16] = (uint32_t)((int32_t)c->r[16] >> 16);
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[31]);
    c->r[31] = 0x800455F4u;
    c->r[4] = c->r[16] + c->r[0]; func_80083E80(c);
    c->r[2] = c->r[0] - c->r[2];
    c->r[17] = c->r[17] << 16;
    c->r[17] = (uint32_t)((int32_t)c->r[17] >> 16);
    { int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[17]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[4] = c->r[16] + c->r[0];
    c->r[8] = c->lo;
    c->r[31] = 0x80045614u;
    c->r[16] = (uint32_t)((int32_t)c->r[8] >> 12); func_80083F50(c);
    { int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[17]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[4] = c->r[19] + c->r[0];
    c->r[2] = (uint32_t)c->mem_r16((c->r[19] + (uint32_t)50));
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = c->r[2] + c->r[18];
    c->mem_w16((c->r[3] + (uint32_t)446), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[19] + (uint32_t)54));
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = c->r[2] + c->r[16];
    c->mem_w16((c->r[3] + (uint32_t)448), (uint16_t)c->r[2]);
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[19] + (uint32_t)46));
    c->r[8] = c->lo;
    c->r[5] = (uint32_t)((int32_t)c->r[8] >> 12);
    c->r[2] = c->r[2] + c->r[5];
    c->r[31] = 0x80045658u;
    c->mem_w16((c->r[3] + (uint32_t)444), (uint16_t)c->r[2]); func_800498C8(c);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + c->r[0]; if (_t) goto L_80045708; }
    c->r[31] = 0x80045668u;
     func_80045724(c);
    { int _t = (c->r[2] == c->r[0]); c->r[7] = (uint32_t)8064u << 16; if (_t) goto L_80045704; }
    c->r[6] = (uint32_t)c->mem_r16((c->r[7] + (uint32_t)422));
    c->r[2] = c->r[0] + (uint32_t)1536;
    c->r[3] = c->r[6] & 3840u;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = (uint32_t)((int32_t)c->r[3] < 1537); if (_t) goto L_800456E4; }
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)256; if (_t) goto L_8004569C; }
    { int _t = (c->r[3] == c->r[2]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_800456B0; }
     goto L_800456EC;
  L_8004569C:;
    c->r[2] = c->r[0] + (uint32_t)1792;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_800456E4; }
     goto L_800456EC;
  L_800456B0:;
    c->r[2] = (uint32_t)32780u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-1933));
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_800456E4; }
    c->r[3] = (uint32_t)8064u << 16;
    c->r[4] = (uint32_t)c->mem_r16((c->r[19] + (uint32_t)50));
    c->r[5] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)452));
    c->r[3] = c->r[6] & 255u;
    c->mem_w16((c->r[7] + (uint32_t)422), (uint16_t)c->r[3]);
    c->r[4] = c->r[4] + c->r[5];
    c->mem_w16((c->r[19] + (uint32_t)50), (uint16_t)c->r[4]); goto L_80045708;
  L_800456E4:;
    c->r[2] = c->r[0] + (uint32_t)2; goto L_80045708;
  L_800456EC:;
    c->r[3] = (uint32_t)c->mem_r16((c->r[19] + (uint32_t)50));
    c->r[4] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)452));
    c->r[2] = c->r[0] + (uint32_t)1;
    c->r[3] = c->r[3] + c->r[4];
    c->mem_w16((c->r[19] + (uint32_t)50), (uint16_t)c->r[3]); goto L_80045708;
  L_80045704:;
    c->r[2] = c->r[0] + c->r[0];
  L_80045708:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)40; return;
    return;
}

static void leaf_8004602C(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-72;
    c->mem_w32((c->r[29] + (uint32_t)48), c->r[20]);
    c->r[20] = c->r[6] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)56), c->r[22]);
    c->r[22] = c->r[4] + c->r[0];
    c->r[8] = c->r[0] + (uint32_t)1792;
    c->mem_w16((c->r[29] + (uint32_t)16), (uint16_t)c->r[5]);
    c->r[5] = c->r[5] << 16;
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[16]);
    c->r[16] = (uint32_t)((int32_t)c->r[5] >> 16);
    c->mem_w32((c->r[29] + (uint32_t)68), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)64), c->r[30]);
    c->mem_w32((c->r[29] + (uint32_t)60), c->r[23]);
    c->mem_w32((c->r[29] + (uint32_t)52), c->r[21]);
    c->mem_w32((c->r[29] + (uint32_t)44), c->r[19]);
    c->mem_w32((c->r[29] + (uint32_t)40), c->r[18]);
    c->mem_w32((c->r[29] + (uint32_t)36), c->r[17]);
    { int _t = (c->r[16] != c->r[0]); c->mem_w16((c->r[29] + (uint32_t)24), (uint16_t)c->r[8]); if (_t) goto L_8004607C; }
    c->mem_w16((c->r[29] + (uint32_t)24), (uint16_t)c->r[4]);
  L_8004607C:;
    c->r[31] = 0x80046084u;
    c->r[17] = c->r[0] + c->r[0]; func_80048360(c);
    c->r[3] = (uint32_t)8064u << 16;
    c->r[4] = (uint32_t)8064u << 16;
    c->r[5] = c->mem_r32((c->r[3] + (uint32_t)480));
    c->r[4] = c->mem_r32((c->r[4] + (uint32_t)472));
    c->r[3] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)2));
    c->r[19] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)4));
    c->r[3] = c->r[3] << 3;
    c->r[4] = c->r[4] + c->r[3];
    c->r[3] = c->r[2] + c->r[0];
    c->r[2] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)6));
    c->r[6] = (uint32_t)8064u << 16;
    c->mem_w32((c->r[6] + (uint32_t)492), c->r[4]);
    { int _t = (c->r[19] == c->r[0]); c->r[5] = c->r[2] >> 8; if (_t) goto L_800461B0; }
    c->r[30] = c->r[6] + c->r[0];
    c->r[2] = c->r[3] << 16;
    c->r[23] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[21] = c->r[5] + c->r[0];
    c->r[18] = c->r[16] + c->r[0];
    c->r[2] = c->r[20] << 16;
    c->r[20] = (uint32_t)((int32_t)c->r[2] >> 16);
  L_800460D8:;
    c->r[2] = c->mem_r32((c->r[30] + (uint32_t)492));
    c->r[3] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)0));
    c->r[2] = c->r[3] & c->r[22];
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80046190; }
    c->r[8] = (uint32_t)c->mem_r16((c->r[29] + (uint32_t)24));
    c->r[2] = c->r[3] & c->r[8];
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[23] + c->r[0]; if (_t) goto L_80046190; }
    c->r[5] = c->r[21] + c->r[0];
    c->r[6] = c->r[18] + c->r[0];
    c->r[31] = 0x80046118u;
    c->r[7] = c->r[20] + c->r[0]; func_800462E4(c);
    { int _t = (c->r[18] == c->r[0]); c->r[4] = c->r[2] + c->r[0]; if (_t) goto L_8004615C; }
    { int _t = ((int32_t)c->r[4] >= 0); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_80046160; }
    c->r[4] = c->r[23] + c->r[0];
    c->r[5] = c->r[21] + c->r[0];
    c->r[6] = c->r[18] + c->r[0];
    c->r[16] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)446));
    c->r[7] = c->r[20] + c->r[0];
    c->r[2] = c->r[2] + (uint32_t)20;
    c->r[31] = 0x8004614Cu;
    c->mem_w16((c->r[16] + (uint32_t)446), (uint16_t)c->r[2]); func_800462E4(c);
    c->r[3] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)446));
    c->r[4] = c->r[2] + c->r[0];
    c->r[3] = c->r[3] + (uint32_t)-20;
    c->mem_w16((c->r[16] + (uint32_t)446), (uint16_t)c->r[3]);
  L_8004615C:;
    c->r[2] = c->r[0] + (uint32_t)1;
  L_80046160:;
    { int _t = (c->r[4] == c->r[2]); c->r[2] = (uint32_t)((int32_t)c->r[4] < 2); if (_t) goto L_8004618C; }
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)2; if (_t) goto L_80046180; }
    { int _t = (c->r[4] == c->r[0]); c->r[2] = c->r[0] + c->r[0]; if (_t) goto L_800462B4; }
     goto L_80046190;
  L_80046180:;
    { int _t = (c->r[4] != c->r[2]);  if (_t) goto L_80046190; }
    c->r[22] = c->r[22] | 32768u;
  L_8004618C:;
    c->r[17] = c->r[19] & 65535u;
  L_80046190:;
    c->r[2] = c->mem_r32((c->r[30] + (uint32_t)492));
    c->r[17] = c->r[17] + (uint32_t)1;
    c->r[2] = c->r[2] + (uint32_t)8;
    c->mem_w32((c->r[30] + (uint32_t)492), c->r[2]);
    c->r[2] = c->r[19] & 65535u;
    c->r[2] = (uint32_t)((int32_t)c->r[17] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_800460D8; }
  L_800461B0:;
    c->r[2] = c->r[19] & 65535u;
    { int _t = (c->r[17] != c->r[2]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_800461C4; }
    c->r[2] = c->r[0] + c->r[0]; goto L_800462B4;
  L_800461C4:;
    c->r[6] = c->mem_r32((c->r[2] + (uint32_t)480));
    c->r[2] = (uint32_t)c->mem_r16((c->r[6] + (uint32_t)0));
    c->r[2] = c->r[2] & 4u;
    { int _t = (c->r[2] == c->r[0]); c->r[4] = (uint32_t)8064u << 16; if (_t) goto L_800461F4; }
    c->r[5] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)454));
    c->r[3] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)450));
    c->mem_w16((c->r[4] + (uint32_t)450), (uint16_t)c->r[2]);
    c->mem_w16((c->r[5] + (uint32_t)454), (uint16_t)c->r[3]);
  L_800461F4:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[6] + (uint32_t)0));
    c->r[2] = c->r[2] & 1u;
    { int _t = (c->r[2] == c->r[0]); c->r[4] = (uint32_t)8064u << 16; if (_t) goto L_80046218; }
    c->r[3] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)454));
    c->r[2] = c->r[0] + (uint32_t)63;
    c->r[2] = c->r[2] - c->r[3];
    c->mem_w16((c->r[4] + (uint32_t)454), (uint16_t)c->r[2]);
  L_80046218:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[6] + (uint32_t)0));
    c->r[2] = c->r[2] & 2u;
    { int _t = (c->r[2] == c->r[0]); c->r[4] = (uint32_t)8064u << 16; if (_t) goto L_8004623C; }
    c->r[3] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)450));
    c->r[2] = c->r[0] + (uint32_t)63;
    c->r[2] = c->r[2] - c->r[3];
    c->mem_w16((c->r[4] + (uint32_t)450), (uint16_t)c->r[2]);
  L_8004623C:;
    c->r[8] = (uint32_t)c->mem_r16((c->r[29] + (uint32_t)16));
    c->r[2] = c->r[8] << 16;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_800462AC; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)422));
    c->r[2] = c->r[2] & 1024u;
    { int _t = (c->r[2] == c->r[0]); c->r[4] = (uint32_t)8064u << 16; if (_t) goto L_80046294; }
    c->r[5] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)446));
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[5] + (uint32_t)420));
    c->r[6] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)446));
    c->r[4] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)420));
    c->r[2] = c->r[2] + (uint32_t)-32;
    c->r[2] = (uint32_t)((int32_t)c->r[2] < (int32_t)c->r[3]);
    { int _t = (c->r[2] == c->r[0]); c->r[3] = (uint32_t)8064u << 16; if (_t) goto L_80046294; }
    c->r[2] = c->r[4] - c->r[6];
    c->mem_w16((c->r[3] + (uint32_t)452), (uint16_t)c->r[2]); goto L_800462AC;
  L_80046294:;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)422));
    c->r[4] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[4] + (uint32_t)452), (uint16_t)c->r[0]);
    c->r[3] = c->r[3] & 64511u;
    c->mem_w16((c->r[2] + (uint32_t)422), (uint16_t)c->r[3]);
  L_800462AC:;
    c->r[2] = c->r[22] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
  L_800462B4:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)68));
    c->r[30] = c->mem_r32((c->r[29] + (uint32_t)64));
    c->r[23] = c->mem_r32((c->r[29] + (uint32_t)60));
    c->r[22] = c->mem_r32((c->r[29] + (uint32_t)56));
    c->r[21] = c->mem_r32((c->r[29] + (uint32_t)52));
    c->r[20] = c->mem_r32((c->r[29] + (uint32_t)48));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)44));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)40));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)36));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[29] = c->r[29] + (uint32_t)72; return;
    return;
}

static void leaf_800490E4(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->r[10] = c->r[4] + c->r[0];
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = c->mem_r32((c->r[3] + (uint32_t)456));
    c->r[7] = c->r[0] + (uint32_t)1;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[31]);
    c->r[9] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)0));
    { int _t = (c->r[9] == c->r[0]); c->r[8] = c->r[2] + (uint32_t)2; if (_t) goto L_80049210; }
    c->r[11] = c->r[3] + c->r[0];
  L_80049110:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[8] + (uint32_t)0));
    c->r[8] = c->r[8] + (uint32_t)2;
    c->r[3] = c->mem_r32((c->r[11] + (uint32_t)456));
    c->r[2] = c->r[2] << 1;
    c->r[6] = c->r[3] + c->r[2];
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[6] + (uint32_t)0));
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[6] + (uint32_t)2));
    c->r[3] = c->r[4] + c->r[0];
    { int _t = (c->r[4] != c->r[2]); c->r[5] = c->r[2] + c->r[0]; if (_t) goto L_80049140; }
    c->r[5] = c->r[5] + (uint32_t)64; goto L_80049154;
  L_80049140:;
    c->r[2] = (uint32_t)((int32_t)c->r[2] < (int32_t)c->r[4]);
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[3] + c->r[0]; if (_t) goto L_80049154; }
    c->r[3] = c->r[5] + c->r[0];
    c->r[5] = c->r[4] + c->r[0];
  L_80049154:;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[4] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)444));
    c->r[3] = c->r[3] + (uint32_t)-128;
    c->r[2] = c->r[5] + (uint32_t)128;
    c->r[2] = c->r[2] - c->r[3];
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[4] = c->r[4] - c->r[3];
    c->r[3] = c->r[4] & 65535u;
    c->r[2] = (uint32_t)((int32_t)c->r[2] < (int32_t)c->r[3]);
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_800491F8; }
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[6] + (uint32_t)4));
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[6] + (uint32_t)6));
    c->r[3] = c->r[4] + c->r[0];
    { int _t = (c->r[4] != c->r[2]); c->r[5] = c->r[2] + c->r[0]; if (_t) goto L_800491A0; }
    c->r[5] = c->r[5] + (uint32_t)64; goto L_800491B4;
  L_800491A0:;
    c->r[2] = (uint32_t)((int32_t)c->r[2] < (int32_t)c->r[4]);
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[3] + c->r[0]; if (_t) goto L_800491B4; }
    c->r[3] = c->r[5] + c->r[0];
    c->r[5] = c->r[4] + c->r[0];
  L_800491B4:;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[4] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)448));
    c->r[3] = c->r[3] + (uint32_t)-128;
    c->r[2] = c->r[5] + (uint32_t)128;
    c->r[2] = c->r[2] - c->r[3];
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[4] = c->r[4] - c->r[3];
    c->r[3] = c->r[4] & 65535u;
    c->r[2] = (uint32_t)((int32_t)c->r[2] < (int32_t)c->r[3]);
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_800491F8; }
    c->mem_w8((c->r[10] + (uint32_t)42), (uint8_t)c->r[7]);
    c->r[31] = 0x800491F0u;
    c->r[4] = c->r[7] & 255u; func_80048ECC(c);
     goto L_80049240;
  L_800491F8:;
    c->r[7] = c->r[7] + (uint32_t)1;
    c->r[3] = c->r[7] & 65535u;
    c->r[2] = c->r[9] & 65535u;
    c->r[2] = (uint32_t)(c->r[2] < c->r[3]);
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80049110; }
  L_80049210:;
    c->r[4] = c->r[0] + (uint32_t)1;
    c->r[2] = c->r[4] + c->r[0];
    c->r[31] = 0x80049220u;
    c->mem_w8((c->r[10] + (uint32_t)42), (uint8_t)c->r[2]); func_80048ECC(c);
    c->r[2] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)434));
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[2] + (uint32_t)444), (uint16_t)c->r[3]);
    c->r[2] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)436));
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[2] + (uint32_t)448), (uint16_t)c->r[3]);
  L_80049240:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_800492B0(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-40;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
    c->r[18] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[17] = c->r[5] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[19]);
    c->r[19] = c->r[6] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[7] << 16;
    c->r[16] = (uint32_t)((int32_t)c->r[16] >> 16);
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[31]);
    c->r[31] = 0x800492E4u;
    c->r[4] = c->r[16] + c->r[0]; func_80083E80(c);
    c->r[2] = c->r[0] - c->r[2];
    c->r[17] = c->r[17] << 16;
    c->r[17] = (uint32_t)((int32_t)c->r[17] >> 16);
    { int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[17]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[4] = c->r[16] + c->r[0];
    c->r[8] = c->lo;
    c->r[31] = 0x80049304u;
    c->r[16] = (uint32_t)((int32_t)c->r[8] >> 12); func_80083F50(c);
    { int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[17]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[4] = c->r[18] + c->r[0];
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)50));
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = c->r[2] + c->r[19];
    c->mem_w16((c->r[3] + (uint32_t)446), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)54));
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = c->r[2] + c->r[16];
    c->mem_w16((c->r[3] + (uint32_t)448), (uint16_t)c->r[2]);
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)46));
    c->r[8] = c->lo;
    c->r[5] = (uint32_t)((int32_t)c->r[8] >> 12);
    c->r[2] = c->r[2] + c->r[5];
    c->r[31] = 0x80049348u;
    c->mem_w16((c->r[3] + (uint32_t)444), (uint16_t)c->r[2]); func_800498C8(c);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + c->r[0]; if (_t) goto L_800493CC; }
    c->r[31] = 0x80049358u;
     func_80045724(c);
    { int _t = (c->r[2] == c->r[0]); c->r[5] = (uint32_t)8064u << 16; if (_t) goto L_800493C8; }
    c->r[4] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)422));
    c->r[2] = c->r[0] + (uint32_t)1536;
    c->r[3] = c->r[4] & 3840u;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = (uint32_t)((int32_t)c->r[3] < 1537); if (_t) goto L_800493C0; }
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)256; if (_t) goto L_8004938C; }
    { int _t = (c->r[3] == c->r[2]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_800493A0; }
     goto L_800493CC;
  L_8004938C:;
    c->r[2] = c->r[0] + (uint32_t)1792;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_800493C0; }
     goto L_800493CC;
  L_800493A0:;
    c->r[2] = (uint32_t)32780u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-1933));
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[4] & 255u; if (_t) goto L_800493C0; }
    c->mem_w16((c->r[5] + (uint32_t)422), (uint16_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)1; goto L_800493CC;
  L_800493C0:;
    c->r[2] = c->r[0] + (uint32_t)2; goto L_800493CC;
  L_800493C8:;
    c->r[2] = c->r[0] + c->r[0];
  L_800493CC:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)40; return;
    return;
}

static void leaf_800493E8(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->r[5] = c->r[5] << 16;
    c->r[6] = c->r[6] << 16;
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16);
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[31]);
    c->r[7] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)86));
    c->r[31] = 0x80049408u;
    c->r[6] = (uint32_t)((int32_t)c->r[6] >> 16); func_80049418(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_8004EAD0(Core* c) {
    c->r[3] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)0));
    c->r[2] = c->r[0] + (uint32_t)1;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = (uint32_t)((int32_t)c->r[3] < 2); if (_t) goto L_8004EB10; }
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)2; if (_t) goto L_8004EAF8; }
    { int _t = (c->r[3] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)240; if (_t) goto L_8004EB08; }
    c->mem_w8((c->r[4] + (uint32_t)0), (uint8_t)c->r[2]); goto L_8004EB20;
  L_8004EAF8:;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = c->r[0] + (uint32_t)240; if (_t) goto L_8004EB18; }
    c->mem_w8((c->r[4] + (uint32_t)0), (uint8_t)c->r[2]); goto L_8004EB20;
  L_8004EB08:;
    c->r[2] = c->r[0] + (uint32_t)244; goto L_8004EB1C;
  L_8004EB10:;
    c->r[2] = c->r[0] + (uint32_t)242; goto L_8004EB1C;
  L_8004EB18:;
    c->r[2] = c->r[0] + (uint32_t)243;
  L_8004EB1C:;
    c->mem_w8((c->r[4] + (uint32_t)0), (uint8_t)c->r[2]);
  L_8004EB20:;
    c->r[5] = c->mem_r32((c->r[5] + (uint32_t)4));
    c->r[2] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)0));
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[4] + (uint32_t)1; if (_t) goto L_8004EB78; }
    c->r[8] = c->r[0] + (uint32_t)10;
    c->r[7] = c->r[0] + (uint32_t)32;
    c->r[6] = c->r[0] + (uint32_t)251;
  L_8004EB44:;
    c->r[3] = c->r[2] & 255u;
    { int _t = (c->r[3] == c->r[8]);  if (_t) goto L_8004EB58; }
    { int _t = (c->r[3] != c->r[7]); c->r[2] = c->r[2] + (uint32_t)-32; if (_t) goto L_8004EB60; }
  L_8004EB58:;
    c->mem_w8((c->r[4] + (uint32_t)0), (uint8_t)c->r[6]); goto L_8004EB64;
  L_8004EB60:;
    c->mem_w8((c->r[4] + (uint32_t)0), (uint8_t)c->r[2]);
  L_8004EB64:;
    c->r[5] = c->r[5] + (uint32_t)1;
    c->r[2] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)0));
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[4] + (uint32_t)1; if (_t) goto L_8004EB44; }
  L_8004EB78:;
    c->r[2] = c->r[0] + (uint32_t)240;
    c->mem_w8((c->r[4] + (uint32_t)0), (uint8_t)c->r[2]);
    c->r[4] = c->r[4] + (uint32_t)1;
    c->r[2] = c->r[0] + (uint32_t)255;
    c->mem_w8((c->r[4] + (uint32_t)0), (uint8_t)c->r[2]);
    c->r[2] = c->r[4] + c->r[0]; return;
    return;
}

static void leaf_8004EE2C(Core* c) {
    c->r[5] = c->r[0] | 65535u;
    c->r[3] = c->r[0] + (uint32_t)11;
    c->r[2] = c->r[4] + (uint32_t)22;
  L_8004EE38:;
    c->mem_w16((c->r[2] + (uint32_t)4), (uint16_t)c->r[5]);
    c->r[3] = c->r[3] + (uint32_t)-1;
    { int _t = ((int32_t)c->r[3] >= 0); c->r[2] = c->r[2] + (uint32_t)-2; if (_t) goto L_8004EE38; }
    c->mem_w32((c->r[4] + (uint32_t)0), c->r[0]); return;
    return;
}

static void leaf_8004EE50(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->r[5] = c->r[0] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[31]);
    c->r[31] = 0x8004EE6Cu;
    c->r[6] = c->r[0] + (uint32_t)140; func_8009A420(c);
    c->r[2] = c->r[0] + (uint32_t)255;
    c->mem_w8((c->r[16] + (uint32_t)61), (uint8_t)c->r[2]);
    c->mem_w8((c->r[16] + (uint32_t)16), (uint8_t)c->r[2]);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_8004F184(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-96;
    c->mem_w32((c->r[29] + (uint32_t)88), c->r[20]);
    c->r[20] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)92), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)84), c->r[19]);
    c->mem_w32((c->r[29] + (uint32_t)80), c->r[18]);
    c->mem_w32((c->r[29] + (uint32_t)76), c->r[17]);
    c->mem_w32((c->r[29] + (uint32_t)72), c->r[16]);
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[20] + (uint32_t)8));
    c->r[19] = c->r[5] + c->r[0];
    c->r[2] = c->r[3] << 3;
    c->r[2] = c->r[2] + c->r[3];
    c->r[2] = c->r[2] << 2;
    c->r[2] = c->r[2] - c->r[3];
    c->r[2] = c->r[2] << 2;
    c->r[2] = c->r[2] + (uint32_t)12;
    c->r[16] = c->r[20] + c->r[2];
    c->r[31] = 0x8004F1D0u;
    c->r[4] = c->r[16] + c->r[0]; func_8004EE50(c);
    c->r[4] = c->mem_r32((c->r[19] + (uint32_t)0));
    { int _t = (c->r[4] != c->r[0]); c->mem_w16((c->r[29] + (uint32_t)64), (uint16_t)c->r[0]); if (_t) goto L_8004F2A4; }
    c->r[18] = c->r[29] + (uint32_t)16;
    c->r[4] = c->r[18] + c->r[0];
    c->r[17] = c->r[19] + (uint32_t)4;
    c->r[5] = c->r[17] + c->r[0];
    c->r[6] = c->r[0] + (uint32_t)2;
    c->r[31] = 0x8004F1FCu;
    c->mem_w8((c->r[16] + (uint32_t)9), (uint8_t)c->r[0]); func_8004F058(c);
    c->r[4] = c->r[2] + c->r[0];
    c->r[2] = c->r[4] << 16;
    { int _t = (c->r[2] == c->r[0]); c->mem_w16((c->r[29] + (uint32_t)64), (uint16_t)c->r[4]); if (_t) goto L_8004F21C; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)9));
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w8((c->r[16] + (uint32_t)9), (uint8_t)c->r[2]);
  L_8004F21C:;
    c->r[4] = c->r[18] + c->r[0];
    c->r[5] = c->r[17] + c->r[0];
    c->r[31] = 0x8004F22Cu;
    c->r[6] = c->r[0] + (uint32_t)1; func_8004F058(c);
    c->r[4] = c->r[2] + c->r[0];
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[29] + (uint32_t)64));
    c->r[3] = c->r[4] << 16;
    c->r[3] = (uint32_t)((int32_t)c->r[3] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < (int32_t)c->r[3]);
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_8004F24C; }
    c->mem_w16((c->r[29] + (uint32_t)64), (uint16_t)c->r[4]);
  L_8004F24C:;
    { int _t = (c->r[3] == c->r[0]); c->r[4] = c->r[18] + c->r[0]; if (_t) goto L_8004F264; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)9));
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w8((c->r[16] + (uint32_t)9), (uint8_t)c->r[2]);
  L_8004F264:;
    c->r[5] = c->r[17] + c->r[0];
    c->r[31] = 0x8004F270u;
    c->r[6] = c->r[0] + c->r[0]; func_8004F058(c);
    c->r[4] = c->r[2] + c->r[0];
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[29] + (uint32_t)64));
    c->r[3] = c->r[4] << 16;
    c->r[3] = (uint32_t)((int32_t)c->r[3] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < (int32_t)c->r[3]);
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_8004F290; }
    c->mem_w16((c->r[29] + (uint32_t)64), (uint16_t)c->r[4]);
  L_8004F290:;
    { int _t = (c->r[3] == c->r[0]); c->r[4] = c->r[16] + (uint32_t)16; if (_t) goto L_8004F2B8; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)9));
    c->r[2] = c->r[2] + (uint32_t)1; goto L_8004F2B0;
  L_8004F2A4:;
    c->r[18] = c->r[4] + c->r[0];
    c->r[31] = 0x8004F2B0u;
    c->r[5] = c->r[29] + (uint32_t)64; func_8004EFC0(c);
  L_8004F2B0:;
    c->mem_w8((c->r[16] + (uint32_t)9), (uint8_t)c->r[2]);
    c->r[4] = c->r[16] + (uint32_t)16;
  L_8004F2B8:;
    c->r[31] = 0x8004F2C0u;
    c->r[5] = c->r[18] + c->r[0]; func_8004EF54(c);
    c->r[4] = (uint32_t)c->mem_r16((c->r[29] + (uint32_t)64));
    c->r[7] = c->mem_r32((c->r[19] + (uint32_t)0));
    c->r[8] = c->mem_r32((c->r[19] + (uint32_t)4));
    c->r[9] = c->mem_r32((c->r[19] + (uint32_t)8));
    c->r[10] = c->mem_r32((c->r[19] + (uint32_t)12));
    c->mem_w32((c->r[16] + (uint32_t)108), c->r[7]);
    c->mem_w32((c->r[16] + (uint32_t)112), c->r[8]);
    c->mem_w32((c->r[16] + (uint32_t)116), c->r[9]);
    c->mem_w32((c->r[16] + (uint32_t)120), c->r[10]);
    c->r[7] = c->mem_r32((c->r[19] + (uint32_t)16));
    c->r[8] = c->mem_r32((c->r[19] + (uint32_t)20));
    c->r[9] = c->mem_r32((c->r[19] + (uint32_t)24));
    c->r[10] = c->mem_r32((c->r[19] + (uint32_t)28));
    c->mem_w32((c->r[16] + (uint32_t)124), c->r[7]);
    c->mem_w32((c->r[16] + (uint32_t)128), c->r[8]);
    c->mem_w32((c->r[16] + (uint32_t)132), c->r[9]);
    c->mem_w32((c->r[16] + (uint32_t)136), c->r[10]);
    c->r[2] = c->r[4] & 1u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[4] << 16; if (_t) goto L_8004F318; }
    c->r[4] = c->r[4] + (uint32_t)1;
    c->r[2] = c->r[4] << 16;
  L_8004F318:;
    c->r[3] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = c->r[2] >> 31;
    c->r[3] = c->r[3] + c->r[2];
    c->r[3] = (uint32_t)((int32_t)c->r[3] >> 1);
    c->r[2] = c->r[0] + (uint32_t)160;
    c->r[2] = c->r[2] - c->r[3];
    c->mem_w16((c->r[16] + (uint32_t)0), (uint16_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)140;
    c->mem_w16((c->r[16] + (uint32_t)2), (uint16_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)8;
    c->mem_w16((c->r[16] + (uint32_t)4), (uint16_t)c->r[4]);
    c->mem_w16((c->r[16] + (uint32_t)6), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[20] + (uint32_t)8));
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w16((c->r[20] + (uint32_t)8), (uint16_t)c->r[2]);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)92));
    c->r[20] = c->mem_r32((c->r[29] + (uint32_t)88));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)84));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)80));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)76));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)72));
    c->r[29] = c->r[29] + (uint32_t)96; return;
    return;
}

static void leaf_800535E0(Core* c) {
    c->r[2] = (uint32_t)32783u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)9232));
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80053654; }
    c->r[3] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)108));
    c->r[2] = (uint32_t)(c->r[3] < (uint32_t)10);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)32769u << 16; if (_t) goto L_80053668; }
    c->r[2] = c->r[2] + (uint32_t)23372;
    c->r[3] = c->r[3] << 2;
    c->r[3] = c->r[3] + c->r[2];
    c->r[2] = c->mem_r32((c->r[3] + (uint32_t)0));
    {  switch (c->r[2]) { case 0x80053624u: goto L_80053624; case 0x8005362Cu: goto L_8005362C; case 0x8005363Cu: goto L_8005363C; case 0x8005364Cu: goto L_8005364C; default: rec_dispatch(c, c->r[2]); return; } }
  L_80053624:;
    c->r[2] = c->r[0] + (uint32_t)1; return;
  L_8005362C:;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)561));
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)1); return;
  L_8005363C:;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)561));
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)2); return;
  L_8005364C:;
    { int _t = (c->r[5] == c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_8005365C; }
  L_80053654:;
    c->r[2] = c->r[0] + c->r[0]; return;
  L_8005365C:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)593));
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)1); return;
  L_80053668:;
    c->r[2] = c->r[0] + c->r[0]; return;
    return;
}

static void leaf_80054790(Core* c) {
    c->r[2] = (uint32_t)32778u << 16;
    c->r[2] = c->r[2] + (uint32_t)17144;
    c->r[5] = c->r[5] + c->r[2];
    c->r[5] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)0));
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)71));
    c->r[6] = c->r[5] & 255u;
    { int _t = (c->r[6] == c->r[2]); c->r[2] = (uint32_t)32783u << 16; if (_t) goto L_80054904; }
    c->r[3] = (uint32_t)32782u << 16;
    c->mem_w8((c->r[4] + (uint32_t)71), (uint8_t)c->r[5]);
    c->r[8] = c->mem_r32((c->r[2] + (uint32_t)-12268));
    c->r[2] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)32766));
    c->r[2] = c->r[2] & 64u;
    { int _t = (c->r[2] != c->r[0]); c->r[7] = c->r[8] + (uint32_t)4; if (_t) goto L_80054844; }
    c->r[2] = (uint32_t)32778u << 16;
    c->r[5] = c->r[2] + (uint32_t)17580;
    c->r[2] = c->r[6] << 1;
    c->r[2] = c->r[2] + c->r[6];
    c->r[5] = c->r[2] + c->r[5];
    c->r[2] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)0));
    c->r[2] = c->r[2] << 2;
    c->r[2] = c->r[2] + c->r[7];
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)0));
    c->r[3] = c->mem_r32((c->r[4] + (uint32_t)196));
    c->r[2] = c->r[8] + c->r[2];
    c->mem_w32((c->r[3] + (uint32_t)64), c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)1));
    c->r[2] = c->r[2] << 2;
    c->r[2] = c->r[2] + c->r[7];
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)0));
    c->r[3] = c->mem_r32((c->r[4] + (uint32_t)208));
    c->r[2] = c->r[8] + c->r[2];
    c->mem_w32((c->r[3] + (uint32_t)64), c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)2));
    c->r[2] = c->r[2] << 2;
    c->r[2] = c->r[2] + c->r[7];
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)0));
    c->r[3] = c->mem_r32((c->r[4] + (uint32_t)220));
    c->r[2] = c->r[8] + c->r[2]; goto L_80054900;
  L_80054844:;
    c->r[2] = c->r[0] + (uint32_t)10;
    { int _t = (c->r[6] != c->r[2]); c->r[2] = (uint32_t)32778u << 16; if (_t) goto L_80054894; }
    c->r[2] = c->mem_r32((c->r[7] + (uint32_t)4));
    c->r[3] = c->mem_r32((c->r[4] + (uint32_t)196));
    c->r[2] = c->r[8] + c->r[2];
    c->mem_w32((c->r[3] + (uint32_t)64), c->r[2]);
    c->r[2] = c->mem_r32((c->r[4] + (uint32_t)220));
    c->mem_w32((c->r[2] + (uint32_t)64), c->r[0]);
    c->r[2] = c->mem_r32((c->r[4] + (uint32_t)208));
    c->mem_w32((c->r[2] + (uint32_t)64), c->r[0]);
    c->r[2] = c->mem_r32((c->r[7] + (uint32_t)76));
    c->r[3] = c->mem_r32((c->r[4] + (uint32_t)204));
    c->r[2] = c->r[8] + c->r[2];
    c->mem_w32((c->r[3] + (uint32_t)64), c->r[2]);
    c->r[2] = c->mem_r32((c->r[7] + (uint32_t)80));
     goto L_800548F8;
  L_80054894:;
    c->r[5] = c->r[2] + (uint32_t)17580;
    c->r[2] = c->r[6] << 1;
    c->r[2] = c->r[2] + c->r[6];
    c->r[5] = c->r[2] + c->r[5];
    c->r[2] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)0));
    c->r[2] = c->r[2] << 2;
    c->r[2] = c->r[2] + c->r[7];
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)0));
    c->r[3] = c->mem_r32((c->r[4] + (uint32_t)196));
    c->r[2] = c->r[8] + c->r[2];
    c->mem_w32((c->r[3] + (uint32_t)64), c->r[2]);
    c->r[2] = c->mem_r32((c->r[7] + (uint32_t)16));
    c->r[3] = c->mem_r32((c->r[4] + (uint32_t)208));
    c->r[2] = c->r[8] + c->r[2];
    c->mem_w32((c->r[3] + (uint32_t)64), c->r[2]);
    c->r[2] = c->mem_r32((c->r[7] + (uint32_t)28));
    c->r[3] = c->mem_r32((c->r[4] + (uint32_t)220));
    c->r[2] = c->r[8] + c->r[2];
    c->mem_w32((c->r[3] + (uint32_t)64), c->r[2]);
    c->r[2] = c->mem_r32((c->r[7] + (uint32_t)12));
    c->r[3] = c->mem_r32((c->r[4] + (uint32_t)204));
    c->r[2] = c->r[8] + c->r[2];
    c->mem_w32((c->r[3] + (uint32_t)64), c->r[2]);
    c->r[2] = c->mem_r32((c->r[7] + (uint32_t)24));
  L_800548F8:;
    c->r[3] = c->mem_r32((c->r[4] + (uint32_t)216));
    c->r[2] = c->r[8] + c->r[2];
  L_80054900:;
    c->mem_w32((c->r[3] + (uint32_t)64), c->r[2]);
  L_80054904:;
     return;
    return;
}

static void leaf_80056EC8(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->r[2] = c->r[0] + (uint32_t)640;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[31]);
    c->r[5] = c->mem_r32((c->r[4] + (uint32_t)260));
    c->r[3] = c->r[0] + (uint32_t)512;
    c->mem_w16((c->r[5] + (uint32_t)10), (uint16_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)3456;
    c->mem_w16((c->r[5] + (uint32_t)56), (uint16_t)c->r[3]);
    c->mem_w16((c->r[5] + (uint32_t)58), (uint16_t)c->r[3]);
    c->mem_w16((c->r[5] + (uint32_t)60), (uint16_t)c->r[3]);
    c->r[5] = c->mem_r32((c->r[4] + (uint32_t)264));
    c->r[4] = c->r[0] + (uint32_t)34;
    c->mem_w16((c->r[5] + (uint32_t)10), (uint16_t)c->r[2]);
    c->mem_w16((c->r[5] + (uint32_t)56), (uint16_t)c->r[3]);
    c->mem_w16((c->r[5] + (uint32_t)58), (uint16_t)c->r[3]);
    c->mem_w16((c->r[5] + (uint32_t)60), (uint16_t)c->r[3]);
    c->r[31] = 0x80056F10u;
    c->r[5] = c->r[0] + c->r[0]; func_800310F4(c);
    c->r[3] = c->r[2] + c->r[0];
    { int _t = (c->r[3] == c->r[0]);  if (_t) goto L_80056F2C; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)40));
    c->r[2] = c->r[2] | 128u;
    c->mem_w8((c->r[3] + (uint32_t)40), (uint8_t)c->r[2]);
  L_80056F2C:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

static void leaf_80057150(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[17] = c->r[5] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[31]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)382));
    c->r[2] = c->r[2] & 64u;
    { int _t = (c->r[2] == c->r[0]); c->r[3] = c->r[0] + c->r[0]; if (_t) goto L_80057208; }
    c->r[4] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)70));
    c->r[2] = c->r[0] + (uint32_t)203;
    { int _t = (c->r[4] != c->r[2]); c->r[2] = c->r[0] + (uint32_t)202; if (_t) goto L_800571BC; }
    c->r[2] = c->mem_r32((c->r[16] + (uint32_t)56));
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)6));
    c->r[2] = c->r[2] & 32768u;
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_80057200; }
    c->r[5] = c->r[0] + (uint32_t)202;
    c->r[31] = 0x800571B4u;
    c->r[6] = c->r[0] + (uint32_t)4; func_80054D14(c);
    c->r[3] = c->r[0] + (uint32_t)2; goto L_8005720C;
  L_800571BC:;
    { int _t = (c->r[4] != c->r[2]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_80057210; }
    c->r[2] = (uint32_t)32782u << 16;
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)32360));
    c->r[3] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)370));
    c->r[2] = c->r[2] & c->r[3];
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_80057208; }
    c->r[5] = c->r[0] + (uint32_t)203;
    c->r[31] = 0x800571F0u;
    c->r[6] = c->r[0] + (uint32_t)4; func_80054D14(c);
    c->r[4] = c->r[0] + (uint32_t)53;
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x80057200u;
    c->r[6] = c->r[5] + c->r[0]; func_80074590(c);
  L_80057200:;
    c->r[3] = c->r[0] + (uint32_t)1; goto L_8005720C;
  L_80057208:;
    c->r[3] = c->r[0] + (uint32_t)2;
  L_8005720C:;
    c->r[2] = c->r[0] + (uint32_t)1;
  L_80057210:;
    { int _t = (c->r[3] != c->r[2]); c->r[2] = c->r[0] + (uint32_t)2; if (_t) goto L_80057220; }
    c->r[2] = c->r[0] + (uint32_t)768; goto L_800572D8;
  L_80057220:;
    { int _t = (c->r[3] != c->r[2]); c->r[2] = c->r[0] + (uint32_t)3584; if (_t) goto L_800572D8; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)326));
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80057248; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)120));
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_8005724C; }
  L_80057248:;
    c->r[17] = c->r[0] + (uint32_t)8;
  L_8005724C:;
    c->r[4] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)330));
    c->r[2] = c->r[4] & 2u;
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[4] & 1u; if (_t) goto L_80057284; }
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)327));
    { int _t = (c->r[2] != c->r[3]); c->r[2] = c->r[0] + (uint32_t)8; if (_t) goto L_80057284; }
    { int _t = (c->r[17] == c->r[2]); c->r[4] = c->r[0] + (uint32_t)96; if (_t) goto L_800572D4; }
    c->r[5] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)88));
    c->r[6] = c->r[0] + (uint32_t)16; goto L_800572C8;
  L_80057284:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)330));
    c->r[2] = c->r[2] & 8u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)8; if (_t) goto L_800572B8; }
    { int _t = (c->r[17] == c->r[2]); c->r[4] = c->r[0] + (uint32_t)3840; if (_t) goto L_800572B0; }
    c->r[5] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)88));
    c->r[31] = 0x800572ACu;
    c->r[6] = c->r[0] + (uint32_t)32; func_800776F8(c);
    c->mem_w16((c->r[16] + (uint32_t)88), (uint16_t)c->r[2]);
  L_800572B0:;
    c->r[2] = c->r[0] + (uint32_t)6656; goto L_800572D8;
  L_800572B8:;
    { int _t = (c->r[17] == c->r[2]); c->r[4] = c->r[0] + c->r[0]; if (_t) goto L_800572D4; }
    c->r[5] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)88));
    c->r[6] = c->r[0] + (uint32_t)64;
  L_800572C8:;
    c->r[31] = 0x800572D0u;
     func_800776F8(c);
    c->mem_w16((c->r[16] + (uint32_t)88), (uint16_t)c->r[2]);
  L_800572D4:;
    c->r[2] = c->r[0] + (uint32_t)3584;
  L_800572D8:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

static void leaf_80072114(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[17] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
    c->r[18] = c->r[5] + c->r[0];
    c->r[5] = c->r[0] + (uint32_t)128;
    c->r[6] = c->r[0] + (uint32_t)3;
    c->r[7] = c->r[0] + (uint32_t)13;
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[31]);
    c->r[31] = 0x80072140u;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]); func_80072DDC(c);
    c->r[16] = c->r[2] + c->r[0];
    { int _t = (c->r[16] == c->r[0]); c->r[2] = (uint32_t)32775u << 16; if (_t) goto L_800721C4; }
    c->r[2] = c->r[2] + (uint32_t)9504;
    c->mem_w32((c->r[16] + (uint32_t)28), c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)2;
    c->mem_w8((c->r[16] + (uint32_t)3), (uint8_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)31870;
    c->mem_w16((c->r[16] + (uint32_t)14), (uint16_t)c->r[18]);
    c->mem_w16((c->r[16] + (uint32_t)92), (uint16_t)c->r[2]);
    c->r[4] = c->r[0] + (uint32_t)8;
    c->r[5] = c->r[0] + (uint32_t)5;
    c->r[31] = 0x80072178u;
    c->r[6] = c->r[0] + c->r[0]; func_80074590(c);
    c->r[2] = (uint32_t)32780u << 16;
    c->r[4] = c->r[2] + (uint32_t)-2040;
    c->r[3] = c->mem_r32((c->r[4] + (uint32_t)52));
    { int _t = (c->r[3] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)3; if (_t) goto L_80072194; }
    c->mem_w8((c->r[3] + (uint32_t)4), (uint8_t)c->r[2]);
  L_80072194:;
    c->r[2] = (uint32_t)32782u << 16;
    c->r[3] = (uint32_t)32780u << 16;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)32750));
    c->r[3] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)-1923));
    c->r[2] = (uint32_t)((int32_t)c->r[2] < (int32_t)c->r[3]);
    { int _t = (c->r[2] != c->r[0]); c->mem_w32((c->r[4] + (uint32_t)52), c->r[16]); if (_t) goto L_800721BC; }
    c->mem_w8((c->r[16] + (uint32_t)7), (uint8_t)c->r[0]); goto L_800721C4;
  L_800721BC:;
    c->r[2] = c->r[0] + (uint32_t)1;
    c->mem_w8((c->r[16] + (uint32_t)7), (uint8_t)c->r[2]);
  L_800721C4:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

static void leaf_80077D64(Core* c) {
    c->r[2] = (uint32_t)32780u << 16;
    c->r[3] = c->r[2] + (uint32_t)-2040;
    c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)58));
    c->r[2] = c->r[2] & 128u;
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)129; if (_t) goto L_80077D84; }
    c->mem_w8((c->r[3] + (uint32_t)58), (uint8_t)c->r[2]);
  L_80077D84:;
     return;
    return;
}

static void leaf_80077E3C(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[17] = c->r[6] + c->r[0];
    c->r[6] = c->r[17] << 16;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[31]);
    c->r[31] = 0x80077E60u;
    c->r[6] = (uint32_t)((int32_t)c->r[6] >> 16); func_80075FF8(c);
    c->r[17] = c->r[17] + (uint32_t)4096;
    c->mem_w16((c->r[16] + (uint32_t)14), (uint16_t)c->r[17]);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

static void leaf_80079324(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->r[9] = c->r[5] << 16;
    c->r[5] = (uint32_t)8u << 16;
    c->r[5] = c->r[5] | 8u;
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = c->r[0] + (uint32_t)-32;
    c->r[4] = c->r[4] << 16;
    c->r[4] = (uint32_t)((int32_t)c->r[4] >> 16);
    c->r[6] = c->r[6] << 16;
    c->r[4] = c->r[4] | c->r[9];
    c->r[8] = c->mem_r32((c->r[29] + (uint32_t)48));
    c->r[6] = (uint32_t)((int32_t)c->r[6] >> 16);
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[31]);
    c->mem_w16((c->r[3] + (uint32_t)384), (uint16_t)c->r[2]);
    c->r[31] = 0x80079364u;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[8]); func_80078CA8(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

static void leaf_8007E6DC(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-40;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->r[8] = (uint32_t)8064u << 16;
    c->r[12] = (uint32_t)32780u << 16;
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[18]);
    c->r[18] = c->r[7] + c->r[0];
    c->r[13] = (uint32_t)32783u << 16;
    c->mem_w32((c->r[29] + (uint32_t)36), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[17]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)0));
    c->r[7] = (uint32_t)c->mem_r16((c->r[18] + (uint32_t)2));
    c->r[4] = c->r[3] & 240u;
    c->r[10] = c->r[4] + c->r[0];
    c->r[2] = c->r[7] & 32768u;
    c->r[2] = c->r[2] << 16;
    c->r[15] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[14] = c->r[4] + c->r[0];
    c->r[3] = c->r[3] & 15u;
    c->mem_w8((c->r[18] + (uint32_t)0), (uint8_t)c->r[3]);
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[5] + (uint32_t)0));
    c->r[7] = c->r[7] & 32767u;
    c->r[2] = c->r[2] << 2;
    c->r[2] = c->r[6] + c->r[2];
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)2));
    c->r[11] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)0));
    c->r[6] = c->r[6] + c->r[3];
    c->r[9] = c->r[6] + (uint32_t)11;
    c->r[24] = (uint32_t)c->mem_r16((c->r[6] + (uint32_t)6));
  L_8007E750:;
    c->r[2] = c->mem_r32((c->r[16] + (uint32_t)0));
    c->mem_w32((c->r[8] + (uint32_t)8), c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[9] + (uint32_t)3));
    c->r[3] = (uint32_t)c->mem_r16((c->r[8] + (uint32_t)8));
    c->r[2] = c->r[2] << 24;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 24);
    c->r[3] = c->r[3] + c->r[2];
    c->mem_w16((c->r[8] + (uint32_t)8), (uint16_t)c->r[3]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[9] + (uint32_t)4));
    c->r[3] = (uint32_t)c->mem_r16((c->r[8] + (uint32_t)10));
    c->r[2] = c->r[2] << 24;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 24);
    c->r[3] = c->r[3] + c->r[2];
    c->mem_w16((c->r[8] + (uint32_t)10), (uint16_t)c->r[3]);
    c->r[2] = c->mem_r32((c->r[6] + (uint32_t)0));
    c->mem_w32((c->r[8] + (uint32_t)12), c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[9] + (uint32_t)-1));
    c->mem_w16((c->r[8] + (uint32_t)16), (uint16_t)c->r[2]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[9] + (uint32_t)0));
    c->r[2] = c->r[0] + (uint32_t)100;
    c->mem_w8((c->r[8] + (uint32_t)7), (uint8_t)c->r[2]);
    { int _t = (c->r[15] == c->r[0]); c->mem_w16((c->r[8] + (uint32_t)18), (uint16_t)c->r[3]); if (_t) goto L_8007E7C0; }
    c->r[2] = c->r[0] + (uint32_t)102;
    c->mem_w8((c->r[8] + (uint32_t)7), (uint8_t)c->r[2]);
  L_8007E7C0:;
    { int _t = (c->r[14] == c->r[0]);  if (_t) goto L_8007E7D8; }
    c->mem_w8((c->r[8] + (uint32_t)6), (uint8_t)c->r[10]);
    c->mem_w8((c->r[8] + (uint32_t)5), (uint8_t)c->r[10]);
    c->mem_w8((c->r[8] + (uint32_t)4), (uint8_t)c->r[10]); goto L_8007E7E8;
  L_8007E7D8:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[8] + (uint32_t)7));
    c->r[2] = c->r[2] | 1u;
    c->mem_w8((c->r[8] + (uint32_t)7), (uint8_t)c->r[2]);
  L_8007E7E8:;
    { int _t = (c->r[7] == c->r[0]);  if (_t) goto L_8007E7FC; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[18] + (uint32_t)2));
    c->mem_w16((c->r[8] + (uint32_t)14), (uint16_t)c->r[2]);
  L_8007E7FC:;
    c->r[4] = c->mem_r32((c->r[12] + (uint32_t)-2748));
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)1));
    c->r[5] = c->mem_r32((c->r[13] + (uint32_t)-10040));
    c->r[2] = c->r[2] << 2;
    c->r[5] = c->r[5] + c->r[2];
    c->r[2] = c->mem_r32((c->r[5] + (uint32_t)0));
    c->r[3] = (uint32_t)1024u << 16;
    c->r[2] = c->r[2] | c->r[3];
    c->mem_w32((c->r[4] + (uint32_t)0), c->r[2]);
    c->mem_w32((c->r[5] + (uint32_t)0), c->r[4]);
    c->r[4] = c->r[4] + (uint32_t)4;
    c->r[9] = c->r[9] + (uint32_t)16;
    c->r[6] = c->r[6] + (uint32_t)16;
    c->r[2] = c->mem_r32((c->r[8] + (uint32_t)4));
    c->r[11] = c->r[11] + (uint32_t)-1;
    c->mem_w32((c->r[4] + (uint32_t)0), c->r[2]);
    c->r[2] = c->mem_r32((c->r[8] + (uint32_t)8));
    c->r[4] = c->r[4] + (uint32_t)4;
    c->mem_w32((c->r[4] + (uint32_t)0), c->r[2]);
    c->r[2] = c->mem_r32((c->r[8] + (uint32_t)12));
    c->r[4] = c->r[4] + (uint32_t)4;
    c->mem_w32((c->r[4] + (uint32_t)0), c->r[2]);
    c->r[2] = c->mem_r32((c->r[8] + (uint32_t)16));
    c->r[4] = c->r[4] + (uint32_t)4;
    c->mem_w32((c->r[4] + (uint32_t)0), c->r[2]);
    c->r[4] = c->r[4] + (uint32_t)4;
    { int _t = (c->r[11] != c->r[0]); c->mem_w32((c->r[12] + (uint32_t)-2748), c->r[4]); if (_t) goto L_8007E750; }
    c->r[17] = (uint32_t)32780u << 16;
    c->r[7] = c->r[24] << 16;
    c->r[5] = c->r[0] + c->r[0];
    c->r[6] = c->r[5] + c->r[0];
    c->r[16] = c->mem_r32((c->r[17] + (uint32_t)-2748));
    c->r[7] = (uint32_t)((int32_t)c->r[7] >> 16);
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[0]);
    c->r[31] = 0x8007E890u;
    c->r[4] = c->r[16] + c->r[0]; func_80083DE0(c);
    c->r[3] = (uint32_t)32783u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[18] + (uint32_t)1));
    c->r[4] = c->mem_r32((c->r[3] + (uint32_t)-10040));
    c->r[2] = c->r[2] << 2;
    c->r[4] = c->r[4] + c->r[2];
    c->r[2] = c->mem_r32((c->r[4] + (uint32_t)0));
    c->r[3] = (uint32_t)512u << 16;
    c->r[2] = c->r[2] | c->r[3];
    c->mem_w32((c->r[16] + (uint32_t)0), c->r[2]);
    c->mem_w32((c->r[4] + (uint32_t)0), c->r[16]);
    c->r[2] = c->mem_r32((c->r[17] + (uint32_t)-2748));
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)36));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[2] = c->r[2] + (uint32_t)12;
    c->mem_w32((c->r[17] + (uint32_t)-2748), c->r[2]);
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[29] = c->r[29] + (uint32_t)40; return;
    return;
}

static void leaf_8007E938(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-40;
    c->r[8] = c->r[4] + c->r[0];
    c->r[2] = (uint32_t)32769u << 16;
    c->r[2] = c->r[2] + (uint32_t)29492;
    c->r[3] = c->mem_r32((c->r[29] + (uint32_t)56));
    c->r[4] = c->r[29] + (uint32_t)24;
    c->mem_w16((c->r[29] + (uint32_t)26), (uint16_t)c->r[6]);
    c->r[6] = c->r[8] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[31]);
    c->mem_w8((c->r[29] + (uint32_t)17), (uint8_t)c->r[7]);
    c->mem_w8((c->r[29] + (uint32_t)16), (uint8_t)c->r[0]);
    c->mem_w16((c->r[29] + (uint32_t)18), (uint16_t)c->r[0]);
    c->mem_w16((c->r[29] + (uint32_t)24), (uint16_t)c->r[5]);
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[0]);
    c->r[3] = c->r[3] << 16;
    c->r[3] = (uint32_t)((int32_t)c->r[3] >> 14);
    c->r[3] = c->r[3] + c->r[2];
    c->r[5] = c->mem_r32((c->r[3] + (uint32_t)0));
    c->r[31] = 0x8007E988u;
    c->r[7] = c->r[29] + (uint32_t)16; func_8007E6DC(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[29] = c->r[29] + (uint32_t)40; return;
    return;
}

static void leaf_8007FC24(Core* c) {
    c->r[3] = (uint32_t)32780u << 16;
    c->r[2] = c->mem_r32((c->r[3] + (uint32_t)-2748));
    c->r[4] = c->r[2] + c->r[0];
    c->r[2] = c->r[2] + (uint32_t)36;
    c->mem_w32((c->r[3] + (uint32_t)-2748), c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)56;
    c->mem_w8((c->r[4] + (uint32_t)7), (uint8_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)70;
    c->mem_w8((c->r[4] + (uint32_t)6), (uint8_t)c->r[2]);
    c->mem_w8((c->r[4] + (uint32_t)14), (uint8_t)c->r[2]);
    c->mem_w8((c->r[4] + (uint32_t)22), (uint8_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)16;
    c->r[3] = c->r[0] + (uint32_t)320;
    c->mem_w8((c->r[4] + (uint32_t)30), (uint8_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)240;
    c->mem_w8((c->r[4] + (uint32_t)4), (uint8_t)c->r[0]);
    c->mem_w8((c->r[4] + (uint32_t)5), (uint8_t)c->r[0]);
    c->mem_w8((c->r[4] + (uint32_t)12), (uint8_t)c->r[0]);
    c->mem_w8((c->r[4] + (uint32_t)13), (uint8_t)c->r[0]);
    c->mem_w8((c->r[4] + (uint32_t)20), (uint8_t)c->r[0]);
    c->mem_w8((c->r[4] + (uint32_t)21), (uint8_t)c->r[0]);
    c->mem_w8((c->r[4] + (uint32_t)28), (uint8_t)c->r[0]);
    c->mem_w8((c->r[4] + (uint32_t)29), (uint8_t)c->r[0]);
    c->mem_w16((c->r[4] + (uint32_t)8), (uint16_t)c->r[0]);
    c->mem_w16((c->r[4] + (uint32_t)10), (uint16_t)c->r[0]);
    c->mem_w16((c->r[4] + (uint32_t)16), (uint16_t)c->r[3]);
    c->mem_w16((c->r[4] + (uint32_t)18), (uint16_t)c->r[0]);
    c->mem_w16((c->r[4] + (uint32_t)24), (uint16_t)c->r[0]);
    c->mem_w16((c->r[4] + (uint32_t)26), (uint16_t)c->r[2]);
    c->mem_w16((c->r[4] + (uint32_t)32), (uint16_t)c->r[3]);
    c->mem_w16((c->r[4] + (uint32_t)34), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)32783u << 16;
    c->r[5] = c->mem_r32((c->r[2] + (uint32_t)-10040));
    c->r[2] = c->mem_r32((c->r[5] + (uint32_t)4));
    c->r[3] = (uint32_t)2048u << 16;
    c->r[2] = c->r[2] | c->r[3];
    c->mem_w32((c->r[4] + (uint32_t)0), c->r[2]);
    c->mem_w32((c->r[5] + (uint32_t)4), c->r[4]); return;
    return;
}

static void leaf_8007FCC8(Core* c) {
    c->r[10] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[2] = c->r[10] & 128u;
    { int _t = (c->r[2] == c->r[0]); c->r[9] = c->r[0] + (uint32_t)1; if (_t) goto L_8007FCE0; }
    c->r[9] = c->r[0] + (uint32_t)2047;
  L_8007FCE0:;
    c->r[2] = (uint32_t)32780u << 16;
    c->r[8] = c->mem_r32((c->r[2] + (uint32_t)-2748));
    c->r[3] = c->r[8] + (uint32_t)16;
    c->mem_w32((c->r[2] + (uint32_t)-2748), c->r[3]);
    c->r[2] = c->r[10] & 127u;
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)70u << 16; if (_t) goto L_8007FD08; }
    c->mem_w32((c->r[8] + (uint32_t)4), c->r[2]); goto L_8007FD0C;
  L_8007FD08:;
    c->mem_w32((c->r[8] + (uint32_t)4), c->r[0]);
  L_8007FD0C:;
    c->r[2] = c->r[0] + (uint32_t)3;
    c->mem_w8((c->r[8] + (uint32_t)3), (uint8_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)96;
    c->mem_w8((c->r[8] + (uint32_t)7), (uint8_t)c->r[2]);
    c->mem_w16((c->r[8] + (uint32_t)8), (uint16_t)c->r[4]);
    c->mem_w16((c->r[8] + (uint32_t)10), (uint16_t)c->r[5]);
    c->mem_w16((c->r[8] + (uint32_t)12), (uint16_t)c->r[6]);
    c->mem_w16((c->r[8] + (uint32_t)14), (uint16_t)c->r[7]);
    c->r[2] = (uint32_t)32783u << 16;
    c->r[4] = c->mem_r32((c->r[2] + (uint32_t)-10040));
    c->r[2] = c->r[9] << 2;
    c->r[4] = c->r[4] + c->r[2];
    c->r[2] = c->mem_r32((c->r[4] + (uint32_t)0));
    c->r[3] = (uint32_t)768u << 16;
    c->r[2] = c->r[2] | c->r[3];
    c->mem_w32((c->r[8] + (uint32_t)0), c->r[2]);
    c->mem_w32((c->r[4] + (uint32_t)0), c->r[8]); return;
    return;
}

static void leaf_80045724(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-40;
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[17] = c->r[0] + (uint32_t)1;
    c->mem_w32((c->r[29] + (uint32_t)36), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[20]);
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[19]);
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
    c->r[31] = 0x80045748u;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]); func_80048360(c);
    c->r[16] = c->r[0] + c->r[0];
    c->r[6] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)8064u << 16;
    c->r[4] = (uint32_t)8064u << 16;
    c->r[5] = c->mem_r32((c->r[3] + (uint32_t)480));
    c->r[4] = c->mem_r32((c->r[4] + (uint32_t)472));
    c->r[3] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)2));
    c->r[19] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)4));
    c->r[3] = c->r[3] << 3;
    c->r[4] = c->r[4] + c->r[3];
    { int _t = (c->r[19] == c->r[0]); c->mem_w32((c->r[6] + (uint32_t)492), c->r[4]); if (_t) goto L_800457D8; }
    c->r[18] = c->r[6] + c->r[0];
    c->r[20] = c->r[2] << 16;
  L_80045780:;
    c->r[2] = c->mem_r32((c->r[18] + (uint32_t)492));
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)0));
    c->r[2] = c->r[2] & 1u;
    { int _t = (c->r[2] == c->r[0]); c->r[4] = (uint32_t)((int32_t)c->r[20] >> 16); if (_t) goto L_800457B8; }
    c->r[31] = 0x800457A4u;
    c->r[5] = c->r[17] + c->r[0]; func_80045810(c);
    c->r[3] = c->r[2] + c->r[0];
    c->r[2] = c->r[0] + (uint32_t)-1;
    { int _t = (c->r[3] != c->r[2]); c->r[2] = c->r[3] + c->r[0]; if (_t) goto L_800457F0; }
    c->r[17] = c->r[0] + c->r[0];
  L_800457B8:;
    c->r[2] = c->mem_r32((c->r[18] + (uint32_t)492));
    c->r[16] = c->r[16] + (uint32_t)1;
    c->r[2] = c->r[2] + (uint32_t)8;
    c->mem_w32((c->r[18] + (uint32_t)492), c->r[2]);
    c->r[2] = c->r[19] & 65535u;
    c->r[2] = (uint32_t)((int32_t)c->r[16] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80045780; }
  L_800457D8:;
    { int _t = (c->r[17] == c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_800457EC; }
    c->mem_w16((c->r[2] + (uint32_t)422), (uint16_t)c->r[0]);
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[2] + (uint32_t)420), (uint16_t)c->r[0]);
  L_800457EC:;
    c->r[2] = c->r[0] + c->r[0];
  L_800457F0:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)36));
    c->r[20] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)40; return;
    return;
}

static void leaf_800462E4(Core* c) {
    c->r[10] = c->r[4] + c->r[0];
    c->r[11] = c->r[5] + c->r[0];
    c->r[2] = (uint32_t)8064u << 16;
    c->r[4] = c->mem_r32((c->r[2] + (uint32_t)492));
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)4));
    c->r[5] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)4));
    { int _t = ((int32_t)c->r[2] <= 0); c->r[9] = c->r[0] + c->r[0]; if (_t) goto L_80046320; }
    c->r[3] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)2));
    c->r[2] = c->r[5] + c->r[0];
    c->r[8] = c->r[3] + c->r[2];
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[2] + (uint32_t)420), (uint16_t)c->r[3]); goto L_80046330;
  L_80046320:;
    c->r[8] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)2));
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = c->r[8] + c->r[5];
    c->mem_w16((c->r[3] + (uint32_t)420), (uint16_t)c->r[2]);
  L_80046330:;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[5] = c->mem_r32((c->r[2] + (uint32_t)492));
    c->r[3] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)0));
    c->r[2] = c->r[3] & 128u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[3] & 64u; if (_t) goto L_80046754; }
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_80046448; }
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)446));
    c->r[2] = c->r[8] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < (int32_t)c->r[3]);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_80046770; }
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)420));
    c->r[2] = (uint32_t)((int32_t)c->r[3] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_80046540; }
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)480));
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)0));
    c->r[2] = c->r[2] & 8u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_800463C4; }
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[5] + (uint32_t)4));
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)450));
    { int64_t _p = (int64_t)(int32_t)c->r[3] * (int64_t)(int32_t)c->r[2]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[3] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)2));
    c->r[12] = c->lo;
    c->r[2] = (uint32_t)((int32_t)c->r[12] >> 6); goto L_8004641C;
  L_800463C4:;
    c->r[3] = c->r[10] << 16;
    c->r[3] = (uint32_t)((int32_t)c->r[3] >> 16);
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)450));
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[5] + (uint32_t)4));
    c->r[2] = c->r[2] - c->r[3];
    { int64_t _p = (int64_t)(int32_t)c->r[4] * (int64_t)(int32_t)c->r[2]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[2] = c->r[10] ^ 63u;
    c->r[3] = c->lo;
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    cpu_div(c, c->r[3], c->r[2]);
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_800463FC; }
    rec_break(c, 7168u);
  L_800463FC:;
    c->r[1] = c->r[0] + (uint32_t)-1;
    { int _t = (c->r[2] != c->r[1]); c->r[1] = (uint32_t)32768u << 16; if (_t) goto L_80046414; }
    { int _t = (c->r[3] != c->r[1]);  if (_t) goto L_80046414; }
    rec_break(c, 6144u);
  L_80046414:;
    c->r[2] = c->lo;
    c->r[3] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)2));
  L_8004641C:;
    c->r[4] = c->r[3] + c->r[2];
    c->r[2] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)446));
    c->r[2] = c->r[4] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[3] = (uint32_t)((int32_t)c->r[3] < (int32_t)c->r[2]);
    { int _t = (c->r[3] == c->r[0]); c->r[2] = c->r[7] << 16; if (_t) goto L_80046538; }
     goto L_80046530;
  L_80046448:;
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)446));
    c->r[2] = c->r[8] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < (int32_t)c->r[3]);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_80046770; }
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)420));
    c->r[2] = (uint32_t)((int32_t)c->r[3] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_80046540; }
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)480));
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)0));
    c->r[2] = c->r[2] & 8u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_800464B4; }
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[5] + (uint32_t)4));
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)450));
    { int64_t _p = (int64_t)(int32_t)c->r[3] * (int64_t)(int32_t)c->r[2]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[3] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)2));
    c->r[12] = c->lo;
    c->r[2] = (uint32_t)((int32_t)c->r[12] >> 6); goto L_8004650C;
  L_800464B4:;
    c->r[3] = c->r[10] << 16;
    c->r[3] = (uint32_t)((int32_t)c->r[3] >> 16);
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)450));
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[5] + (uint32_t)4));
    c->r[2] = c->r[2] - c->r[3];
    { int64_t _p = (int64_t)(int32_t)c->r[4] * (int64_t)(int32_t)c->r[2]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[2] = c->r[10] ^ 63u;
    c->r[3] = c->lo;
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    cpu_div(c, c->r[3], c->r[2]);
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_800464EC; }
    rec_break(c, 7168u);
  L_800464EC:;
    c->r[1] = c->r[0] + (uint32_t)-1;
    { int _t = (c->r[2] != c->r[1]); c->r[1] = (uint32_t)32768u << 16; if (_t) goto L_80046504; }
    { int _t = (c->r[3] != c->r[1]);  if (_t) goto L_80046504; }
    rec_break(c, 6144u);
  L_80046504:;
    c->r[2] = c->lo;
    c->r[3] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)2));
  L_8004650C:;
    c->r[4] = c->r[3] + c->r[2];
    c->r[2] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)446));
    c->r[2] = c->r[4] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < (int32_t)c->r[3]);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[7] << 16; if (_t) goto L_80046538; }
  L_80046530:;
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80046770; }
  L_80046538:;
    c->r[9] = c->r[0] + (uint32_t)1;
    c->r[2] = c->r[0] + (uint32_t)1;
  L_80046540:;
    { int _t = (c->r[9] != c->r[2]); c->r[2] = c->r[6] << 16; if (_t) goto L_800468A4; }
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_800465A0; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)422));
    c->r[2] = c->r[2] & 1024u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_800465A0; }
    c->r[4] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)446));
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)420));
    c->r[4] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)420));
    c->r[2] = c->r[2] + (uint32_t)-32;
    c->r[2] = (uint32_t)((int32_t)c->r[2] < (int32_t)c->r[3]);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_800465A0; }
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)492));
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)2));
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = c->r[4] - c->r[2];
    c->mem_w16((c->r[3] + (uint32_t)452), (uint16_t)c->r[2]); goto L_800465C0;
  L_800465A0:;
    c->r[4] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[3] = c->mem_r32((c->r[2] + (uint32_t)492));
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)446));
    c->r[3] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)2));
    c->r[4] = (uint32_t)8064u << 16;
    c->r[2] = c->r[2] - c->r[3];
    c->mem_w16((c->r[4] + (uint32_t)452), (uint16_t)c->r[2]);
  L_800465C0:;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)480));
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)0));
    c->r[2] = c->r[2] & 8u;
    { int _t = (c->r[2] == c->r[0]); c->r[3] = (uint32_t)8064u << 16; if (_t) goto L_80046660; }
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)492));
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[3] + (uint32_t)452));
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)4));
    c->r[3] = c->r[3] << 6;
    cpu_div(c, c->r[3], c->r[2]);
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80046604; }
    rec_break(c, 7168u);
  L_80046604:;
    c->r[1] = c->r[0] + (uint32_t)-1;
    { int _t = (c->r[2] != c->r[1]); c->r[1] = (uint32_t)32768u << 16; if (_t) goto L_8004661C; }
    { int _t = (c->r[3] != c->r[1]);  if (_t) goto L_8004661C; }
    rec_break(c, 6144u);
  L_8004661C:;
    c->r[3] = c->lo;
    c->r[4] = c->r[11] << 16;
    c->r[4] = (uint32_t)((int32_t)c->r[4] >> 16);
    c->r[2] = c->r[10] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[4] = c->r[4] - c->r[2];
    c->r[2] = c->r[3] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    { int64_t _p = (int64_t)(int32_t)c->r[4] * (int64_t)(int32_t)c->r[2]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[2] + (uint32_t)450), (uint16_t)c->r[3]);
    c->r[3] = (uint32_t)8064u << 16;
    c->r[8] = c->lo;
    c->r[2] = (uint32_t)((int32_t)c->r[8] >> 6);
    c->r[2] = c->r[10] + c->r[2];
    c->mem_w16((c->r[3] + (uint32_t)454), (uint16_t)c->r[2]); goto L_80046718;
  L_80046660:;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[5] = c->r[10] ^ 63u;
    c->r[5] = c->r[5] << 16;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)452));
    c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16);
    { int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[5]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)492));
    c->r[8] = c->lo;
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)4));
    cpu_div(c, c->r[8], c->r[3]);
    { int _t = (c->r[3] != c->r[0]);  if (_t) goto L_8004669C; }
    rec_break(c, 7168u);
  L_8004669C:;
    c->r[1] = c->r[0] + (uint32_t)-1;
    { int _t = (c->r[3] != c->r[1]); c->r[1] = (uint32_t)32768u << 16; if (_t) goto L_800466B4; }
    { int _t = (c->r[8] != c->r[1]);  if (_t) goto L_800466B4; }
    rec_break(c, 6144u);
  L_800466B4:;
    c->r[3] = c->lo;
    c->r[4] = c->r[11] << 16;
    c->r[4] = (uint32_t)((int32_t)c->r[4] >> 16);
    c->r[3] = c->r[10] + c->r[3];
    c->r[2] = c->r[3] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    { int64_t _p = (int64_t)(int32_t)c->r[4] * (int64_t)(int32_t)c->r[2]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[8] = c->lo;
    cpu_div(c, c->r[8], c->r[5]);
    { int _t = (c->r[5] != c->r[0]);  if (_t) goto L_800466EC; }
    rec_break(c, 7168u);
  L_800466EC:;
    c->r[1] = c->r[0] + (uint32_t)-1;
    { int _t = (c->r[5] != c->r[1]); c->r[1] = (uint32_t)32768u << 16; if (_t) goto L_80046704; }
    { int _t = (c->r[8] != c->r[1]);  if (_t) goto L_80046704; }
    rec_break(c, 6144u);
  L_80046704:;
    c->r[5] = c->lo;
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[2] + (uint32_t)450), (uint16_t)c->r[3]);
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[2] + (uint32_t)454), (uint16_t)c->r[5]);
  L_80046718:;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[5] = c->mem_r32((c->r[2] + (uint32_t)492));
    c->r[2] = c->r[0] + (uint32_t)1;
    c->r[3] = (uint32_t)8064u << 16;
    c->r[4] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)0));
    c->r[5] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)6));
    c->r[6] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[3] + (uint32_t)422), (uint16_t)c->r[4]);
    c->r[4] = (uint32_t)8064u << 16;
    c->r[3] = c->r[5] << (c->r[2] & 31);
  L_80046740:;
    c->r[4] = c->mem_r32((c->r[4] + (uint32_t)476));
    c->r[3] = c->r[3] + c->r[5];
    c->r[4] = c->r[4] + c->r[3];
    c->mem_w32((c->r[6] + (uint32_t)488), c->r[4]); return;
  L_80046754:;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)446));
    c->r[2] = c->r[8] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[2] = (uint32_t)((int32_t)c->r[2] < (int32_t)c->r[4]);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_80046778; }
  L_80046770:;
    c->r[2] = c->r[0] + c->r[0]; return;
  L_80046778:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)420));
    c->r[2] = (uint32_t)((int32_t)c->r[4] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[3] & 32u; if (_t) goto L_800468A4; }
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[3] & 64u; if (_t) goto L_8004681C; }
    { int _t = (c->r[2] == c->r[0]); c->r[3] = (uint32_t)8064u << 16; if (_t) goto L_800467B4; }
    c->r[2] = c->r[0] + (uint32_t)64;
    c->mem_w16((c->r[3] + (uint32_t)450), (uint16_t)c->r[2]);
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = c->r[11] + (uint32_t)1;
    c->mem_w16((c->r[3] + (uint32_t)454), (uint16_t)c->r[2]); goto L_80046814;
  L_800467B4:;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)480));
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)0));
    c->r[2] = c->r[2] & 8u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)-1; if (_t) goto L_800467E8; }
    c->mem_w16((c->r[3] + (uint32_t)450), (uint16_t)c->r[2]);
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = c->r[10] + (uint32_t)-1;
    c->mem_w16((c->r[3] + (uint32_t)454), (uint16_t)c->r[2]); goto L_80046814;
  L_800467E8:;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[3] = c->r[10] + (uint32_t)-1;
    c->mem_w16((c->r[2] + (uint32_t)450), (uint16_t)c->r[3]);
    c->r[2] = c->r[11] << 16;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)-1; if (_t) goto L_8004680C; }
    c->r[3] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[3] + (uint32_t)454), (uint16_t)c->r[2]); goto L_80046814;
  L_8004680C:;
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[2] + (uint32_t)454), (uint16_t)c->r[0]);
  L_80046814:;
    c->r[9] = c->r[0] + (uint32_t)2; goto L_80046878;
  L_8004681C:;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_80046838; }
    c->r[3] = c->r[0] + (uint32_t)63;
    c->mem_w16((c->r[2] + (uint32_t)450), (uint16_t)c->r[3]);
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[2] + (uint32_t)454), (uint16_t)c->r[11]); goto L_80046874;
  L_80046838:;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)480));
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)0));
    c->r[2] = c->r[2] & 8u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_80046868; }
    c->mem_w16((c->r[2] + (uint32_t)450), (uint16_t)c->r[0]);
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[2] + (uint32_t)454), (uint16_t)c->r[10]); goto L_80046874;
  L_80046868:;
    c->mem_w16((c->r[2] + (uint32_t)450), (uint16_t)c->r[10]);
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[2] + (uint32_t)454), (uint16_t)c->r[0]);
  L_80046874:;
    c->r[9] = c->r[0] + (uint32_t)1;
  L_80046878:;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[5] = c->mem_r32((c->r[2] + (uint32_t)492));
    c->r[2] = c->r[9] + c->r[0];
    c->r[3] = (uint32_t)8064u << 16;
    c->r[4] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)0));
    c->r[5] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)6));
    c->r[6] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[3] + (uint32_t)422), (uint16_t)c->r[4]);
    c->r[4] = (uint32_t)8064u << 16;
    c->r[3] = c->r[5] << 1; goto L_80046740;
  L_800468A4:;
    c->r[2] = c->r[0] + (uint32_t)-1; return;
    return;
}

static void leaf_80049418(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-40;
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[19]);
    c->r[19] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[17] = c->r[5] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
    c->r[18] = c->r[6] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[7] << 16;
    c->r[16] = (uint32_t)((int32_t)c->r[16] >> 16);
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[31]);
    c->r[31] = 0x8004944Cu;
    c->r[4] = c->r[16] + c->r[0]; func_80083E80(c);
    c->r[2] = c->r[0] - c->r[2];
    c->r[17] = c->r[17] << 16;
    c->r[17] = (uint32_t)((int32_t)c->r[17] >> 16);
    { int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[17]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[4] = c->r[16] + c->r[0];
    c->r[8] = c->lo;
    c->r[31] = 0x8004946Cu;
    c->r[16] = (uint32_t)((int32_t)c->r[8] >> 12); func_80083F50(c);
    { int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[17]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[4] = c->r[19] + c->r[0];
    c->r[2] = (uint32_t)c->mem_r16((c->r[19] + (uint32_t)50));
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = c->r[2] + c->r[18];
    c->mem_w16((c->r[3] + (uint32_t)446), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[19] + (uint32_t)54));
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = c->r[2] + c->r[16];
    c->mem_w16((c->r[3] + (uint32_t)448), (uint16_t)c->r[2]);
    c->r[3] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[19] + (uint32_t)46));
    c->r[8] = c->lo;
    c->r[5] = (uint32_t)((int32_t)c->r[8] >> 12);
    c->r[2] = c->r[2] + c->r[5];
    c->r[31] = 0x800494B0u;
    c->mem_w16((c->r[3] + (uint32_t)444), (uint16_t)c->r[2]); func_800498C8(c);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + c->r[0]; if (_t) goto L_80049500; }
    c->r[31] = 0x800494C0u;
     func_80049800(c);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_800494FC; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)422));
    c->r[3] = c->r[0] + (uint32_t)256;
    c->r[2] = c->r[2] & 3840u;
    { int _t = (c->r[2] != c->r[3]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_800494E4; }
    c->r[2] = c->r[0] + (uint32_t)2; goto L_80049500;
  L_800494E4:;
    c->r[3] = (uint32_t)c->mem_r16((c->r[19] + (uint32_t)50));
    c->r[4] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)452));
    c->r[2] = c->r[0] + (uint32_t)1;
    c->r[3] = c->r[3] + c->r[4];
    c->mem_w16((c->r[19] + (uint32_t)50), (uint16_t)c->r[3]); goto L_80049500;
  L_800494FC:;
    c->r[2] = c->r[0] + c->r[0];
  L_80049500:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)40; return;
    return;
}

static void leaf_8004EFC0(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-48;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[18]);
    c->r[18] = c->r[5] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[17]);
    c->r[17] = c->r[0] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)36), c->r[19]);
    c->r[19] = c->r[0] + (uint32_t)255;
    c->mem_w32((c->r[29] + (uint32_t)40), c->r[31]);
    c->mem_w16((c->r[18] + (uint32_t)0), (uint16_t)c->r[0]);
  L_8004EFEC:;
    c->r[4] = c->r[16] + c->r[0];
    c->r[31] = 0x8004EFF8u;
    c->r[5] = c->r[29] + (uint32_t)16; func_8004EA4C(c);
    c->r[4] = c->r[2] + c->r[0];
    c->r[3] = (uint32_t)c->mem_r8((c->r[29] + (uint32_t)16));
    c->r[2] = c->r[4] << 16;
    c->r[16] = c->r[16] + c->r[3];
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[18] + (uint32_t)0));
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[3] = (uint32_t)((int32_t)c->r[3] < (int32_t)c->r[2]);
    { int _t = (c->r[3] == c->r[0]);  if (_t) goto L_8004F020; }
    c->mem_w16((c->r[18] + (uint32_t)0), (uint16_t)c->r[4]);
  L_8004F020:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)0));
    { int _t = (c->r[2] == c->r[19]); c->r[17] = c->r[17] + (uint32_t)1; if (_t) goto L_8004F038; }
    c->r[16] = c->r[16] + (uint32_t)1; goto L_8004EFEC;
  L_8004F038:;
    c->r[2] = c->r[17] & 255u;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)40));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)36));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[29] = c->r[29] + (uint32_t)48; return;
    return;
}

static void leaf_80045810(Core* c) {
    c->r[6] = c->r[4] + c->r[0];
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)480));
    c->r[7] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)0));
    c->r[9] = (uint32_t)c->mem_r16((c->r[7] + (uint32_t)420));
    c->r[2] = c->r[2] & 8u;
    { int _t = (c->r[2] == c->r[0]); c->r[10] = c->r[5] + c->r[0]; if (_t) goto L_80045868; }
    c->r[2] = (uint32_t)8064u << 16;
    c->r[4] = c->mem_r32((c->r[2] + (uint32_t)492));
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)450));
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)4));
    { int64_t _p = (int64_t)(int32_t)c->r[3] * (int64_t)(int32_t)c->r[2]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[2] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)2));
    c->r[11] = c->lo;
    c->r[3] = (uint32_t)((int32_t)c->r[11] >> 6);
    c->r[2] = c->r[2] + c->r[3];
    c->mem_w16((c->r[7] + (uint32_t)420), (uint16_t)c->r[2]); goto L_800458D8;
  L_80045868:;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[4] = (uint32_t)8064u << 16;
    c->r[3] = c->r[6] << 16;
    c->r[3] = (uint32_t)((int32_t)c->r[3] >> 16);
    c->r[5] = c->mem_r32((c->r[2] + (uint32_t)492));
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)450));
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[5] + (uint32_t)4));
    c->r[2] = c->r[2] - c->r[3];
    { int64_t _p = (int64_t)(int32_t)c->r[4] * (int64_t)(int32_t)c->r[2]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[2] = c->r[6] ^ 63u;
    c->r[3] = c->lo;
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    cpu_div(c, c->r[3], c->r[2]);
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_800458AC; }
    rec_break(c, 7168u);
  L_800458AC:;
    c->r[1] = c->r[0] + (uint32_t)-1;
    { int _t = (c->r[2] != c->r[1]); c->r[1] = (uint32_t)32768u << 16; if (_t) goto L_800458C4; }
    { int _t = (c->r[3] != c->r[1]);  if (_t) goto L_800458C4; }
    rec_break(c, 6144u);
  L_800458C4:;
    c->r[2] = c->lo;
    c->r[3] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)2));
    c->r[3] = c->r[3] + c->r[2];
    c->mem_w16((c->r[7] + (uint32_t)420), (uint16_t)c->r[3]);
  L_800458D8:;
    c->r[3] = (uint32_t)8064u << 16;
    c->r[8] = (uint32_t)8064u << 16;
    c->r[5] = (uint32_t)(int16_t)c->mem_r16((c->r[8] + (uint32_t)420));
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[3] + (uint32_t)446));
    c->r[7] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)446));
    c->r[6] = (uint32_t)c->mem_r16((c->r[8] + (uint32_t)420));
    c->r[2] = c->r[5] + (uint32_t)128;
    c->r[2] = (uint32_t)((int32_t)c->r[2] < (int32_t)c->r[4]);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)((int32_t)c->r[4] < (int32_t)c->r[5]); if (_t) goto L_80045960; }
    { int _t = (c->r[10] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_8004594C; }
    c->r[4] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)8064u << 16;
    c->r[5] = c->mem_r32((c->r[3] + (uint32_t)492));
    c->r[3] = c->r[6] - c->r[7];
    c->mem_w16((c->r[4] + (uint32_t)452), (uint16_t)c->r[3]);
    c->r[3] = (uint32_t)8064u << 16;
    c->r[4] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)0));
    c->r[5] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)6));
    c->r[6] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[3] + (uint32_t)422), (uint16_t)c->r[4]);
    c->r[4] = (uint32_t)8064u << 16;
    c->r[3] = c->r[5] << (c->r[2] & 31);
    c->r[4] = c->mem_r32((c->r[4] + (uint32_t)476));
  L_8004593C:;
    c->r[3] = c->r[3] + c->r[5];
    c->r[4] = c->r[4] + c->r[3];
    c->mem_w32((c->r[6] + (uint32_t)488), c->r[4]); return;
  L_8004594C:;
    c->r[2] = c->r[0] + c->r[0];
    c->r[3] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[8] + (uint32_t)420), (uint16_t)c->r[9]);
    c->mem_w16((c->r[3] + (uint32_t)422), (uint16_t)c->r[0]); return;
  L_80045960:;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)-1; if (_t) goto L_80045988; }
    c->r[3] = (uint32_t)8064u << 16;
    c->r[6] = (uint32_t)8064u << 16;
    c->r[3] = c->mem_r32((c->r[3] + (uint32_t)492));
    c->r[4] = (uint32_t)8064u << 16;
    c->r[5] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)6));
    c->r[4] = c->mem_r32((c->r[4] + (uint32_t)476));
    c->r[3] = c->r[5] << 1; goto L_8004593C;
  L_80045988:;
    c->r[2] = c->r[0] + (uint32_t)1;
    c->r[4] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)8064u << 16;
    c->r[5] = c->mem_r32((c->r[3] + (uint32_t)492));
    c->r[3] = c->r[6] - c->r[7];
    c->mem_w16((c->r[4] + (uint32_t)452), (uint16_t)c->r[3]);
    c->r[3] = (uint32_t)8064u << 16;
    c->r[4] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)0));
    c->r[5] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)6));
    c->r[6] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[3] + (uint32_t)422), (uint16_t)c->r[4]);
    c->r[4] = (uint32_t)8064u << 16;
    c->r[3] = c->r[5] << (c->r[2] & 31);
    c->r[4] = c->mem_r32((c->r[4] + (uint32_t)476));
    c->r[3] = c->r[3] + c->r[5];
    c->r[4] = c->r[4] + c->r[3];
    c->mem_w32((c->r[6] + (uint32_t)488), c->r[4]); return;
    return;
}

static void leaf_80049800(Core* c) {
    c->r[29] = c->r[29] + (uint32_t)-40;
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[19]);
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[31] = 0x8004981Cu;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]); func_80048360(c);
    c->r[16] = c->r[0] + c->r[0];
    c->r[6] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)8064u << 16;
    c->r[4] = (uint32_t)8064u << 16;
    c->r[5] = c->mem_r32((c->r[3] + (uint32_t)480));
    c->r[4] = c->mem_r32((c->r[4] + (uint32_t)472));
    c->r[3] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)2));
    c->r[18] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)4));
    c->r[3] = c->r[3] << 3;
    c->r[4] = c->r[4] + c->r[3];
    { int _t = (c->r[18] == c->r[0]); c->mem_w32((c->r[6] + (uint32_t)492), c->r[4]); if (_t) goto L_800498A8; }
    c->r[17] = c->r[6] + c->r[0];
    c->r[19] = c->r[2] << 16;
  L_80049854:;
    c->r[2] = c->mem_r32((c->r[17] + (uint32_t)492));
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)0));
    c->r[2] = c->r[2] & 2u;
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80049888; }
    c->r[31] = 0x80049878u;
    c->r[4] = (uint32_t)((int32_t)c->r[19] >> 16); func_800459D0(c);
    c->r[3] = c->r[2] + c->r[0];
    c->r[2] = c->r[0] + (uint32_t)-1;
    { int _t = (c->r[3] != c->r[2]); c->r[2] = c->r[3] + c->r[0]; if (_t) goto L_800498AC; }
  L_80049888:;
    c->r[2] = c->mem_r32((c->r[17] + (uint32_t)492));
    c->r[16] = c->r[16] + (uint32_t)1;
    c->r[2] = c->r[2] + (uint32_t)8;
    c->mem_w32((c->r[17] + (uint32_t)492), c->r[2]);
    c->r[2] = c->r[18] & 65535u;
    c->r[2] = (uint32_t)((int32_t)c->r[16] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80049854; }
  L_800498A8:;
    c->r[2] = c->r[0] + c->r[0];
  L_800498AC:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)40; return;
    return;
}

static void leaf_800459D0(Core* c) {
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)492));
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)4));
    { int _t = ((int32_t)c->r[3] <= 0); c->r[6] = c->r[4] + c->r[0]; if (_t) goto L_80045A00; }
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)2));
    c->r[4] = c->r[2] + c->r[3];
    c->r[5] = c->r[2] + c->r[0]; goto L_80045A0C;
  L_80045A00:;
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)2));
    c->r[5] = c->r[4] + c->r[3];
  L_80045A0C:;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)446));
    c->r[2] = (uint32_t)((int32_t)c->r[4] < (int32_t)c->r[3]);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[5] + (uint32_t)-128; if (_t) goto L_80045A2C; }
  L_80045A24:;
    c->r[2] = c->r[0] + c->r[0]; return;
  L_80045A2C:;
    c->r[2] = (uint32_t)((int32_t)c->r[3] < (int32_t)c->r[2]);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_80045B28; }
    c->r[2] = c->mem_r32((c->r[2] + (uint32_t)480));
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)0));
    c->r[2] = c->r[2] & 8u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_80045A7C; }
    c->r[4] = c->mem_r32((c->r[2] + (uint32_t)492));
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)450));
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)4));
    { int64_t _p = (int64_t)(int32_t)c->r[3] * (int64_t)(int32_t)c->r[2]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[3] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)2));
    c->r[8] = c->lo;
    c->r[2] = (uint32_t)((int32_t)c->r[8] >> 6); goto L_80045ADC;
  L_80045A7C:;
    c->r[4] = (uint32_t)8064u << 16;
    c->r[3] = c->r[6] << 16;
    c->r[3] = (uint32_t)((int32_t)c->r[3] >> 16);
    c->r[5] = c->mem_r32((c->r[2] + (uint32_t)492));
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)450));
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[5] + (uint32_t)4));
    c->r[2] = c->r[2] - c->r[3];
    { int64_t _p = (int64_t)(int32_t)c->r[4] * (int64_t)(int32_t)c->r[2]; c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }
    c->r[2] = c->r[6] ^ 63u;
    c->r[3] = c->lo;
    c->r[2] = c->r[2] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    cpu_div(c, c->r[3], c->r[2]);
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80045ABC; }
    rec_break(c, 7168u);
  L_80045ABC:;
    c->r[1] = c->r[0] + (uint32_t)-1;
    { int _t = (c->r[2] != c->r[1]); c->r[1] = (uint32_t)32768u << 16; if (_t) goto L_80045AD4; }
    { int _t = (c->r[3] != c->r[1]);  if (_t) goto L_80045AD4; }
    rec_break(c, 6144u);
  L_80045AD4:;
    c->r[2] = c->lo;
    c->r[3] = (uint32_t)c->mem_r16((c->r[5] + (uint32_t)2));
  L_80045ADC:;
    c->r[6] = c->r[3] + c->r[2];
    c->r[2] = (uint32_t)8064u << 16;
    c->r[3] = c->r[6] << 16;
    c->r[3] = (uint32_t)((int32_t)c->r[3] >> 16);
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)446));
    c->r[7] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)446));
    c->r[3] = (uint32_t)((int32_t)c->r[3] < (int32_t)c->r[4]);
    { int _t = (c->r[3] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_80045A24; }
    c->r[5] = (uint32_t)8064u << 16;
    c->r[3] = (uint32_t)8064u << 16;
    c->r[4] = c->mem_r32((c->r[3] + (uint32_t)492));
    c->r[3] = c->r[6] - c->r[7];
    c->mem_w16((c->r[5] + (uint32_t)452), (uint16_t)c->r[3]);
    c->r[4] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)0));
    c->r[3] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[3] + (uint32_t)422), (uint16_t)c->r[4]); return;
  L_80045B28:;
    c->r[2] = c->r[0] + (uint32_t)-1; return;
    return;
}
}

void register_field_owned_leaves() {
  using overrides::install;
  install(0x80023528u,"leaf_80023528",leaf_80023528,gen_func_80023528,shard_set_override);
  install(0x800248D0u,"leaf_800248D0",leaf_800248D0,gen_func_800248D0,shard_set_override);
  install(0x80024E00u,"leaf_80024E00",leaf_80024E00,gen_func_80024E00,shard_set_override);
  install(0x80024F18u,"leaf_80024F18",leaf_80024F18,gen_func_80024F18,shard_set_override);
  install(0x80025D98u,"leaf_80025D98",leaf_80025D98,gen_func_80025D98,shard_set_override);
  install(0x80028E10u,"leaf_80028E10",leaf_80028E10,gen_func_80028E10,shard_set_override);
  install(0x8002AE0Cu,"leaf_8002AE0C",leaf_8002AE0C,gen_func_8002AE0C,shard_set_override);
  install(0x8002F514u,"leaf_8002F514",leaf_8002F514,gen_func_8002F514,shard_set_override);
  install(0x800317CCu,"leaf_800317CC",leaf_800317CC,gen_func_800317CC,shard_set_override);
  install(0x800329E0u,"leaf_800329E0",leaf_800329E0,gen_func_800329E0,shard_set_override);
  install(0x80033AFCu,"leaf_80033AFC",leaf_80033AFC,gen_func_80033AFC,shard_set_override);
  install(0x8003A290u,"leaf_8003A290",leaf_8003A290,gen_func_8003A290,shard_set_override);
  install(0x8003A3E8u,"leaf_8003A3E8",leaf_8003A3E8,gen_func_8003A3E8,shard_set_override);
  install(0x8003A470u,"leaf_8003A470",leaf_8003A470,gen_func_8003A470,shard_set_override);
  install(0x8003A5E4u,"leaf_8003A5E4",leaf_8003A5E4,gen_func_8003A5E4,shard_set_override);
  install(0x8003A790u,"leaf_8003A790",leaf_8003A790,gen_func_8003A790,shard_set_override);
  install(0x8003A9A0u,"leaf_8003A9A0",leaf_8003A9A0,gen_func_8003A9A0,shard_set_override);
  install(0x8003ABE4u,"leaf_8003ABE4",leaf_8003ABE4,gen_func_8003ABE4,shard_set_override);
  install(0x8003B588u,"leaf_8003B588",leaf_8003B588,gen_func_8003B588,shard_set_override);
  install(0x8003E030u,"leaf_8003E030",leaf_8003E030,gen_func_8003E030,shard_set_override);
  install(0x8003E264u,"leaf_8003E264",leaf_8003E264,gen_func_8003E264,shard_set_override);
  install(0x8003E448u,"leaf_8003E448",leaf_8003E448,gen_func_8003E448,shard_set_override);
  install(0x8003E894u,"leaf_8003E894",leaf_8003E894,gen_func_8003E894,shard_set_override);
  install(0x8003EA88u,"leaf_8003EA88",leaf_8003EA88,gen_func_8003EA88,shard_set_override);
  install(0x8003EBE0u,"leaf_8003EBE0",leaf_8003EBE0,gen_func_8003EBE0,shard_set_override);
  install(0x8003F024u,"leaf_8003F024",leaf_8003F024,gen_func_8003F024,shard_set_override);
  install(0x8003FA1Cu,"leaf_8003FA1C",leaf_8003FA1C,gen_func_8003FA1C,shard_set_override);
  install(0x8003FA44u,"leaf_8003FA44",leaf_8003FA44,gen_func_8003FA44,shard_set_override);
  install(0x8003FB84u,"leaf_8003FB84",leaf_8003FB84,gen_func_8003FB84,shard_set_override);
  install(0x8003FB94u,"leaf_8003FB94",leaf_8003FB94,gen_func_8003FB94,shard_set_override);
  install(0x8003FBC4u,"leaf_8003FBC4",leaf_8003FBC4,gen_func_8003FBC4,shard_set_override);
  install(0x8003FC00u,"leaf_8003FC00",leaf_8003FC00,gen_func_8003FC00,shard_set_override);
  install(0x8003FC78u,"leaf_8003FC78",leaf_8003FC78,gen_func_8003FC78,shard_set_override);
  install(0x8003FC8Cu,"leaf_8003FC8C",leaf_8003FC8C,gen_func_8003FC8C,shard_set_override);
  install(0x8003FE00u,"leaf_8003FE00",leaf_8003FE00,gen_func_8003FE00,shard_set_override);
  install(0x8003FED8u,"leaf_8003FED8",leaf_8003FED8,gen_func_8003FED8,shard_set_override);
  install(0x80041768u,"leaf_80041768",leaf_80041768,gen_func_80041768,shard_set_override);
  install(0x80044CD4u,"leaf_80044CD4",leaf_80044CD4,gen_func_80044CD4,shard_set_override);
  install(0x80045CACu,"leaf_80045CAC",leaf_80045CAC,gen_func_80045CAC,shard_set_override);
  install(0x800468ACu,"leaf_800468AC",leaf_800468AC,gen_func_800468AC,shard_set_override);
  install(0x80047778u,"leaf_80047778",leaf_80047778,gen_func_80047778,shard_set_override);
  install(0x80047B5Cu,"leaf_80047B5C",leaf_80047B5C,gen_func_80047B5C,shard_set_override);
  install(0x80048654u,"leaf_80048654",leaf_80048654,gen_func_80048654,shard_set_override);
  install(0x800489E4u,"leaf_800489E4",leaf_800489E4,gen_func_800489E4,shard_set_override);
  install(0x80048B30u,"leaf_80048B30",leaf_80048B30,gen_func_80048B30,shard_set_override);
  install(0x80048ECCu,"leaf_80048ECC",leaf_80048ECC,gen_func_80048ECC,shard_set_override);
  install(0x80048FC4u,"leaf_80048FC4",leaf_80048FC4,gen_func_80048FC4,shard_set_override);
  install(0x80049250u,"leaf_80049250",leaf_80049250,gen_func_80049250,shard_set_override);
  install(0x80049280u,"leaf_80049280",leaf_80049280,gen_func_80049280,shard_set_override);
  install(0x8004954Cu,"leaf_8004954C",leaf_8004954C,gen_func_8004954C,shard_set_override);
  install(0x80049674u,"leaf_80049674",leaf_80049674,gen_func_80049674,shard_set_override);
  install(0x80049760u,"leaf_80049760",leaf_80049760,gen_func_80049760,shard_set_override);
  install(0x80049F80u,"leaf_80049F80",leaf_80049F80,gen_func_80049F80,shard_set_override);
  install(0x8004A118u,"leaf_8004A118",leaf_8004A118,gen_func_8004A118,shard_set_override);
  install(0x8004A2A0u,"leaf_8004A2A0",leaf_8004A2A0,gen_func_8004A2A0,shard_set_override);
  install(0x8004B428u,"leaf_8004B428",leaf_8004B428,gen_func_8004B428,shard_set_override);
  install(0x8004C0E4u,"leaf_8004C0E4",leaf_8004C0E4,gen_func_8004C0E4,shard_set_override);
  install(0x8004CBD8u,"leaf_8004CBD8",leaf_8004CBD8,gen_func_8004CBD8,shard_set_override);
  install(0x8004D514u,"leaf_8004D514",leaf_8004D514,gen_func_8004D514,shard_set_override);
  install(0x8004D650u,"leaf_8004D650",leaf_8004D650,gen_func_8004D650,shard_set_override);
  install(0x8004D714u,"leaf_8004D714",leaf_8004D714,gen_func_8004D714,shard_set_override);
  install(0x8004D79Cu,"leaf_8004D79C",leaf_8004D79C,gen_func_8004D79C,shard_set_override);
  install(0x8004D8B0u,"leaf_8004D8B0",leaf_8004D8B0,gen_func_8004D8B0,shard_set_override);
  install(0x8004DAECu,"leaf_8004DAEC",leaf_8004DAEC,gen_func_8004DAEC,shard_set_override);
  install(0x8004ED0Cu,"leaf_8004ED0C",leaf_8004ED0C,gen_func_8004ED0C,shard_set_override);
  install(0x8004EE88u,"leaf_8004EE88",leaf_8004EE88,gen_func_8004EE88,shard_set_override);
  install(0x8004EF54u,"leaf_8004EF54",leaf_8004EF54,gen_func_8004EF54,shard_set_override);
  install(0x8004EF8Cu,"leaf_8004EF8C",leaf_8004EF8C,gen_func_8004EF8C,shard_set_override);
  install(0x8004F058u,"leaf_8004F058",leaf_8004F058,gen_func_8004F058,shard_set_override);
  install(0x8004F378u,"leaf_8004F378",leaf_8004F378,gen_func_8004F378,shard_set_override);
  install(0x8004F430u,"leaf_8004F430",leaf_8004F430,gen_func_8004F430,shard_set_override);
  install(0x8004F474u,"leaf_8004F474",leaf_8004F474,gen_func_8004F474,shard_set_override);
  install(0x8004F514u,"leaf_8004F514",leaf_8004F514,gen_func_8004F514,shard_set_override);
  install(0x8004F6D0u,"leaf_8004F6D0",leaf_8004F6D0,gen_func_8004F6D0,shard_set_override);
  install(0x80050894u,"leaf_80050894",leaf_80050894,gen_func_80050894,shard_set_override);
  install(0x800521F4u,"leaf_800521F4",leaf_800521F4,gen_func_800521F4,shard_set_override);
  install(0x8005245Cu,"leaf_8005245C",leaf_8005245C,gen_func_8005245C,shard_set_override);
  install(0x800525D0u,"leaf_800525D0",leaf_800525D0,gen_func_800525D0,shard_set_override);
  install(0x8005262Cu,"leaf_8005262C",leaf_8005262C,gen_func_8005262C,shard_set_override);
  install(0x80052694u,"leaf_80052694",leaf_80052694,gen_func_80052694,shard_set_override);
  install(0x80052720u,"leaf_80052720",leaf_80052720,gen_func_80052720,shard_set_override);
  install(0x8005314Cu,"leaf_8005314C",leaf_8005314C,gen_func_8005314C,shard_set_override);
  install(0x800532A0u,"leaf_800532A0",leaf_800532A0,gen_func_800532A0,shard_set_override);
  install(0x800538E0u,"leaf_800538E0",leaf_800538E0,gen_func_800538E0,shard_set_override);
  install(0x80053D0Cu,"leaf_80053D0C",leaf_80053D0C,gen_func_80053D0C,shard_set_override);
  install(0x80053D90u,"leaf_80053D90",leaf_80053D90,gen_func_80053D90,shard_set_override);
  install(0x800541F4u,"leaf_800541F4",leaf_800541F4,gen_func_800541F4,shard_set_override);
  install(0x800543C0u,"leaf_800543C0",leaf_800543C0,gen_func_800543C0,shard_set_override);
  install(0x8005444Cu,"leaf_8005444C",leaf_8005444C,gen_func_8005444C,shard_set_override);
  install(0x80054E80u,"leaf_80054E80",leaf_80054E80,gen_func_80054E80,shard_set_override);
  install(0x800551C4u,"leaf_800551C4",leaf_800551C4,gen_func_800551C4,shard_set_override);
  install(0x80055284u,"leaf_80055284",leaf_80055284,gen_func_80055284,shard_set_override);
  install(0x80055634u,"leaf_80055634",leaf_80055634,gen_func_80055634,shard_set_override);
  install(0x80055824u,"leaf_80055824",leaf_80055824,gen_func_80055824,shard_set_override);
  install(0x80055D5Cu,"leaf_80055D5C",leaf_80055D5C,gen_func_80055D5C,shard_set_override);
  install(0x80055E28u,"leaf_80055E28",leaf_80055E28,gen_func_80055E28,shard_set_override);
  install(0x80055F48u,"leaf_80055F48",leaf_80055F48,gen_func_80055F48,shard_set_override);
  install(0x80055FBCu,"leaf_80055FBC",leaf_80055FBC,gen_func_80055FBC,shard_set_override);
  install(0x80056C00u,"leaf_80056C00",leaf_80056C00,gen_func_80056C00,shard_set_override);
  install(0x80056D44u,"leaf_80056D44",leaf_80056D44,gen_func_80056D44,shard_set_override);
  install(0x80056E08u,"leaf_80056E08",leaf_80056E08,gen_func_80056E08,shard_set_override);
  install(0x80056F3Cu,"leaf_80056F3C",leaf_80056F3C,gen_func_80056F3C,shard_set_override);
  install(0x8005706Cu,"leaf_8005706C",leaf_8005706C,gen_func_8005706C,shard_set_override);
  install(0x800572ECu,"leaf_800572EC",leaf_800572EC,gen_func_800572EC,shard_set_override);
  install(0x8005749Cu,"leaf_8005749C",leaf_8005749C,gen_func_8005749C,shard_set_override);
  install(0x800574E0u,"leaf_800574E0",leaf_800574E0,gen_func_800574E0,shard_set_override);
  install(0x80057A68u,"leaf_80057A68",leaf_80057A68,gen_func_80057A68,shard_set_override);
  install(0x80057C08u,"leaf_80057C08",leaf_80057C08,gen_func_80057C08,shard_set_override);
  install(0x80059C60u,"leaf_80059C60",leaf_80059C60,gen_func_80059C60,shard_set_override);
  install(0x8005A714u,"leaf_8005A714",leaf_8005A714,gen_func_8005A714,shard_set_override);
  install(0x80062D8Cu,"leaf_80062D8C",leaf_80062D8C,gen_func_80062D8C,shard_set_override);
  install(0x80067EF4u,"leaf_80067EF4",leaf_80067EF4,gen_func_80067EF4,shard_set_override);
  install(0x80067FE4u,"leaf_80067FE4",leaf_80067FE4,gen_func_80067FE4,shard_set_override);
  install(0x80068214u,"leaf_80068214",leaf_80068214,gen_func_80068214,shard_set_override);
  install(0x800682C4u,"leaf_800682C4",leaf_800682C4,gen_func_800682C4,shard_set_override);
  install(0x8006CE74u,"leaf_8006CE74",leaf_8006CE74,gen_func_8006CE74,shard_set_override);
  install(0x8006CEC4u,"leaf_8006CEC4",leaf_8006CEC4,gen_func_8006CEC4,shard_set_override);
  install(0x8006F138u,"leaf_8006F138",leaf_8006F138,gen_func_8006F138,shard_set_override);
  install(0x800708B4u,"leaf_800708B4",leaf_800708B4,gen_func_800708B4,shard_set_override);
  install(0x800716B4u,"leaf_800716B4",leaf_800716B4,gen_func_800716B4,shard_set_override);
  install(0x800737F8u,"leaf_800737F8",leaf_800737F8,gen_func_800737F8,shard_set_override);
  install(0x800738B0u,"leaf_800738B0",leaf_800738B0,gen_func_800738B0,shard_set_override);
  install(0x8007413Cu,"leaf_8007413C",leaf_8007413C,gen_func_8007413C,shard_set_override);
  install(0x80074B44u,"leaf_80074B44",leaf_80074B44,gen_func_80074B44,shard_set_override);
  install(0x80074E48u,"leaf_80074E48",leaf_80074E48,gen_func_80074E48,shard_set_override);
  install(0x800753ACu,"leaf_800753AC",leaf_800753AC,gen_func_800753AC,shard_set_override);
  install(0x8007566Cu,"leaf_8007566C",leaf_8007566C,gen_func_8007566C,shard_set_override);
  install(0x80075D58u,"leaf_80075D58",leaf_80075D58,gen_func_80075D58,shard_set_override);
  install(0x80075FF8u,"leaf_80075FF8",leaf_80075FF8,gen_func_80075FF8,shard_set_override);
  install(0x800776F8u,"leaf_800776F8",leaf_800776F8,gen_func_800776F8,shard_set_override);
  install(0x800782B0u,"leaf_800782B0",leaf_800782B0,gen_func_800782B0,shard_set_override);
  install(0x80078798u,"leaf_80078798",leaf_80078798,gen_func_80078798,shard_set_override);
  install(0x80078824u,"leaf_80078824",leaf_80078824,gen_func_80078824,shard_set_override);
  install(0x800793C4u,"leaf_800793C4",leaf_800793C4,gen_func_800793C4,shard_set_override);
  install(0x80079464u,"leaf_80079464",leaf_80079464,gen_func_80079464,shard_set_override);
  install(0x8007982Cu,"leaf_8007982C",leaf_8007982C,gen_func_8007982C,shard_set_override);
  install(0x8007A810u,"leaf_8007A810",leaf_8007A810,gen_func_8007A810,shard_set_override);
  install(0x8007A8E0u,"leaf_8007A8E0",leaf_8007A8E0,gen_func_8007A8E0,shard_set_override);
  install(0x8007B0F0u,"leaf_8007B0F0",leaf_8007B0F0,gen_func_8007B0F0,shard_set_override);
  install(0x8007B38Cu,"leaf_8007B38C",leaf_8007B38C,gen_func_8007B38C,shard_set_override);
  install(0x8007B45Cu,"leaf_8007B45C",leaf_8007B45C,gen_func_8007B45C,shard_set_override);
  install(0x8007BE18u,"leaf_8007BE18",leaf_8007BE18,gen_func_8007BE18,shard_set_override);
  install(0x8007BF20u,"leaf_8007BF20",leaf_8007BF20,gen_func_8007BF20,shard_set_override);
  install(0x8007E8DCu,"leaf_8007E8DC",leaf_8007E8DC,gen_func_8007E8DC,shard_set_override);
  install(0x8007E998u,"leaf_8007E998",leaf_8007E998,gen_func_8007E998,shard_set_override);
  install(0x8007ED5Cu,"leaf_8007ED5C",leaf_8007ED5C,gen_func_8007ED5C,shard_set_override);
  install(0x8007EE74u,"leaf_8007EE74",leaf_8007EE74,gen_func_8007EE74,shard_set_override);
  install(0x8007EF60u,"leaf_8007EF60",leaf_8007EF60,gen_func_8007EF60,shard_set_override);
  install(0x8007F078u,"leaf_8007F078",leaf_8007F078,gen_func_8007F078,shard_set_override);
  install(0x8007F104u,"leaf_8007F104",leaf_8007F104,gen_func_8007F104,shard_set_override);
  install(0x8007F250u,"leaf_8007F250",leaf_8007F250,gen_func_8007F250,shard_set_override);
  install(0x8007F498u,"leaf_8007F498",leaf_8007F498,gen_func_8007F498,shard_set_override);
  install(0x8007F73Cu,"leaf_8007F73C",leaf_8007F73C,gen_func_8007F73C,shard_set_override);
  install(0x8007F8F8u,"leaf_8007F8F8",leaf_8007F8F8,gen_func_8007F8F8,shard_set_override);
  install(0x8007FD54u,"leaf_8007FD54",leaf_8007FD54,gen_func_8007FD54,shard_set_override);
  install(0x80022D08u,"leaf_80022D08",leaf_80022D08,gen_func_80022D08,shard_set_override);
  install(0x80025744u,"leaf_80025744",leaf_80025744,gen_func_80025744,shard_set_override);
  install(0x80025934u,"leaf_80025934",leaf_80025934,gen_func_80025934,shard_set_override);
  install(0x80025B78u,"leaf_80025B78",leaf_80025B78,gen_func_80025B78,shard_set_override);
  install(0x80027768u,"leaf_80027768",leaf_80027768,gen_func_80027768,shard_set_override);
  install(0x8003A1E4u,"leaf_8003A1E4",leaf_8003A1E4,gen_func_8003A1E4,shard_set_override);
  install(0x8003D23Cu,"leaf_8003D23C",leaf_8003D23C,gen_func_8003D23C,shard_set_override);
  install(0x800455C0u,"leaf_800455C0",leaf_800455C0,gen_func_800455C0,shard_set_override);
  install(0x8004602Cu,"leaf_8004602C",leaf_8004602C,gen_func_8004602C,shard_set_override);
  install(0x800490E4u,"leaf_800490E4",leaf_800490E4,gen_func_800490E4,shard_set_override);
  install(0x800492B0u,"leaf_800492B0",leaf_800492B0,gen_func_800492B0,shard_set_override);
  install(0x800493E8u,"leaf_800493E8",leaf_800493E8,gen_func_800493E8,shard_set_override);
  install(0x8004EAD0u,"leaf_8004EAD0",leaf_8004EAD0,gen_func_8004EAD0,shard_set_override);
  install(0x8004EE2Cu,"leaf_8004EE2C",leaf_8004EE2C,gen_func_8004EE2C,shard_set_override);
  install(0x8004EE50u,"leaf_8004EE50",leaf_8004EE50,gen_func_8004EE50,shard_set_override);
  install(0x8004F184u,"leaf_8004F184",leaf_8004F184,gen_func_8004F184,shard_set_override);
  install(0x800535E0u,"leaf_800535E0",leaf_800535E0,gen_func_800535E0,shard_set_override);
  install(0x80054790u,"leaf_80054790",leaf_80054790,gen_func_80054790,shard_set_override);
  install(0x80056EC8u,"leaf_80056EC8",leaf_80056EC8,gen_func_80056EC8,shard_set_override);
  install(0x80057150u,"leaf_80057150",leaf_80057150,gen_func_80057150,shard_set_override);
  install(0x80072114u,"leaf_80072114",leaf_80072114,gen_func_80072114,shard_set_override);
  install(0x80077D64u,"leaf_80077D64",leaf_80077D64,gen_func_80077D64,shard_set_override);
  install(0x80077E3Cu,"leaf_80077E3C",leaf_80077E3C,gen_func_80077E3C,shard_set_override);
  install(0x80079324u,"leaf_80079324",leaf_80079324,gen_func_80079324,shard_set_override);
  install(0x8007E6DCu,"leaf_8007E6DC",leaf_8007E6DC,gen_func_8007E6DC,shard_set_override);
  install(0x8007E938u,"leaf_8007E938",leaf_8007E938,gen_func_8007E938,shard_set_override);
  install(0x8007FC24u,"leaf_8007FC24",leaf_8007FC24,gen_func_8007FC24,shard_set_override);
  install(0x8007FCC8u,"leaf_8007FCC8",leaf_8007FCC8,gen_func_8007FCC8,shard_set_override);
  install(0x80045724u,"leaf_80045724",leaf_80045724,gen_func_80045724,shard_set_override);
  install(0x800462E4u,"leaf_800462E4",leaf_800462E4,gen_func_800462E4,shard_set_override);
  install(0x80049418u,"leaf_80049418",leaf_80049418,gen_func_80049418,shard_set_override);
  install(0x8004EFC0u,"leaf_8004EFC0",leaf_8004EFC0,gen_func_8004EFC0,shard_set_override);
  install(0x80045810u,"leaf_80045810",leaf_80045810,gen_func_80045810,shard_set_override);
  install(0x80049800u,"leaf_80049800",leaf_80049800,gen_func_80049800,shard_set_override);
  install(0x800459D0u,"leaf_800459D0",leaf_800459D0,gen_func_800459D0,shard_set_override);
}
