#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "axp2101.hpp"
#include "battery_curve.hpp"
#include "check.hpp"

namespace {

using Charge = hg::BatteryPercent::Charge;

// Nox, 2026-10-08 22:40:27 to 2026-10-09 04:03:28: seconds since unplugging and
// battery millivolts, every 30 s while connected, from full until shutdown.
struct Sample {
  uint32_t t;
  uint16_t mv;
};
const Sample kNight[] = {
    {0,4105},{33,4109},{63,4092},{93,4088},{124,4080},{5017,3873},{5048,3868},{5083,3869},{6711,3825},{6742,3825},
    {6777,3827},{6811,3811},{6841,3823},{6877,3824},{6907,3822},{6937,3801},{6972,3819},{7002,3819},{7032,3818},
    {7067,3816},{7097,3813},{7127,3815},{7157,3813},{7187,3813},{7217,3808},{7247,3810},{7283,3809},{7318,3807},
    {7353,3807},{7387,3807},{7417,3789},{7447,3804},{7478,3803},{7508,3797},{7538,3801},{7568,3799},{7603,3798},
    {7638,3797},{7668,3795},{7698,3785},{7728,3793},{7758,3792},{7788,3791},{7824,3790},{7858,3788},{7888,3787},
    {7918,3783},{7949,3785},{7979,3782},{8009,3777},{8039,3782},{8069,3779},{8104,3764},{8134,3777},{8169,3777},
    {8199,3774},{8234,3770},{8264,3773},{8294,3772},{8329,3771},{8359,3767},{8389,3760},{8420,3767},{8455,3754},
    {8485,3764},{8515,3764},{8550,3762},{8580,3760},{8610,3758},{8645,3751},{8675,3757},{8710,3754},{8740,3754},
    {8770,3753},{8800,3751},{8830,3735},{8860,3749},{8895,3747},{8926,3745},{8961,3744},{8991,3742},{9021,3738},
    {9056,3740},{9086,3739},{9116,3735},{9146,3735},{9181,3735},{9216,3734},{9246,3732},{9276,3730},{9307,3730},
    {9341,3723},{9376,3726},{9406,3725},{9437,3709},{9472,3723},{9502,3722},{9537,3713},{9572,3719},{9602,3717},
    {9637,3708},{9672,3714},{9702,3714},{9737,3698},{9767,3711},{9797,3709},{9828,3707},{9862,3707},{9893,3705},
    {9923,3695},{9958,3703},{9993,3702},{10028,3694},{10063,3696},{10093,3696},{10123,3693},{10158,3695},
    {10188,3693},{10218,3692},{10253,3690},{10288,3690},{10319,3676},{10353,3687},{10383,3683},{10418,3671},
    {10448,3682},{10484,3666},{10514,3669},{10544,3679},{10574,3677},{10604,3669},{10639,3675},{10669,3675},
    {10704,3658},{10739,3672},{10769,3670},{10799,3669},{10829,3669},{10860,3668},{10894,3665},{10925,3665},
    {10959,3663},{10989,3653},{11020,3647},{11050,3661},{11085,3658},{11120,3658},{11150,3657},{11180,3655},
    {11215,3655},{11245,3652},{11280,3654},{11315,3637},{11345,3642},{11375,3649},{11410,3649},{11445,3648},
    {11476,3647},{11506,3643},{11541,3644},{11571,3645},{11606,3639},{11636,3641},{11671,3641},{11706,3634},
    {11736,3637},{11766,3637},{11801,3636},{11831,3621},{11861,3618},{11891,3632},{11926,3632},{11961,3623},
    {11992,3631},{12027,3629},{12057,3628},{12092,3627},{12127,3626},{12162,3626},{12192,3625},{12227,3614},
    {12257,3622},{12292,3621},{12322,3621},{12357,3620},{12393,3604},{12427,3618},{12462,3617},{12492,3615},
    {12528,3616},{12563,3609},{12598,3614},{12633,3612},{12663,3612},{12693,3593},{12723,3610},{12758,3609},
    {12788,3608},{12818,3602},{12848,3607},{12883,3606},{12913,3601},{12943,3605},{12974,3603},{13008,3601},
    {13038,3602},{13074,3593},{13104,3600},{13139,3600},{13169,3593},{13199,3598},{13234,3595},{13264,3595},
    {13299,3595},{13329,3595},{13359,3594},{13394,3593},{13424,3592},{13454,3575},{13484,3590},{13519,3589},
    {13550,3589},{13580,3588},{13610,3587},{13640,3587},{13675,3586},{13705,3584},{13740,3584},{13770,3583},
    {13805,3574},{13840,3582},{13875,3581},{13911,3579},{13945,3562},{13976,3576},{14006,3575},{14041,3573},
    {14076,3573},{14111,3573},{14141,3571},{14171,3571},{14206,3555},{14236,3569},{14271,3569},{14306,3569},
    {14336,3567},{14367,3566},{14397,3566},{14431,3547},{14461,3546},{14491,3545},{14522,3561},{14556,3558},
    {14587,3557},{14617,3557},{14647,3542},{14682,3555},{14712,3539},{14742,3554},{14777,3536},{14807,3535},
    {14837,3533},{14867,3529},{14898,3529},{14932,3526},{14962,3525},{14997,3525},{15027,3523},{15059,3521},
    {15093,3522},{15128,3537},{15158,3535},{15188,3517},{15219,3525},{15253,3533},{15283,3532},{15313,3532},
    {15348,3530},{15378,3529},{15413,3528},{15443,3525},{15479,3527},{15513,3524},{15544,3524},{15578,3505},
    {15609,3515},{15644,3520},{15674,3506},{15704,3519},{15734,3519},{15764,3512},{15794,3518},{15824,3517},
    {15854,3516},{15884,3511},{15919,3515},{15954,3513},{15985,3512},{16020,3508},{16055,3509},{16085,3509},
    {16120,3507},{16155,3503},{16185,3506},{16215,3503},{16245,3503},{16275,3502},{16305,3489},{16335,3499},
    {16365,3497},{16395,3483},{16425,3498},{16460,3496},{16495,3494},{16530,3493},{16560,3491},{16591,3482},
    {16621,3490},{16656,3473},{16686,3487},{16716,3487},{16746,3484},{16781,3482},{16811,3482},{16846,3480},
    {16876,3477},{16906,3479},{16936,3476},{16966,3473},{16997,3472},{17031,3470},{17066,3470},{17097,3469},
    {17132,3465},{17162,3465},{17192,3464},{17227,3460},{17257,3442},{17292,3435},{17327,3457},{17357,3453},
    {17387,3454},{17417,3434},{17447,3452},{17477,3441},{17507,3449},{17538,3448},{17572,3442},{17602,3441},
    {17633,3444},{17663,3421},{17693,3423},{17723,3435},{17758,3434},{17793,3431},{17828,3431},{17859,3426},
    {17893,3424},{17928,3422},{17958,3403},{17990,3411},{18024,3412},{18058,3408},{18088,3388},{18119,3401},
    {18164,3393},{18199,3392},{18234,3391},{18264,3386},{18299,3383},{18334,3382},{18365,3379},{18399,3374},
    {18434,3361},{18464,3362},{18494,3361},{18529,3338},{18560,3341},{18594,3344},{18624,3338},{18655,3332},
    {18685,3323},{18720,3319},{18755,3312},{18785,3302},{18820,3281},{18850,3286},{18880,3276},{18910,3267},
    {18940,3257},{18970,3237},{19000,3237},{19030,3225},{19060,3214},{19090,3197},{19120,3188},{19150,3173},
    {19180,3157},{19218,3133},{19251,3120},{19281,3101},{19311,3083},{19346,3057},{19381,3021},
};
constexpr uint32_t kShutdown = 19381;

// Charging from empty, 2026-10-09 04:32:54 until charge done at 07:05:18 (the
// charge curve's source), and 2026-10-08 20:11 until charge done at 22:05:34,
// which the curve wasn't fitted to. Seconds since plugging in, millivolts.
const Sample kChargeFromEmpty[] = {
    {0,3316},{30,3357},{60,3386},{90,3412},{125,3439},{155,3457},{186,3474},{220,3493},
    {255,3509},{285,3523},{315,3535},{346,3546},{380,3557},{411,3564},{441,3569},{471,3574},
    {501,3578},{531,3581},{566,3583},{596,3587},{631,3591},{666,3593},{701,3596},{732,3599},
    {766,3602},{801,3604},{836,3607},{872,3610},{906,3612},{937,3614},{972,3617},{1002,3620},
    {1032,3621},{1067,3624},{1097,3626},{1127,3628},{1157,3631},{1187,3633},{1222,3636},
    {1252,3639},{1287,3641},{1317,3643},{1347,3645},{1382,3648},{1417,3651},{1447,3654},
    {1478,3655},{1508,3659},{1543,3662},{1573,3664},{1603,3666},{1633,3668},{1663,3671},
    {1698,3674},{1934,3691},{1964,3692},{1999,3694},{2029,3696},{2059,3698},{2089,3699},
    {2124,3701},{2154,3704},{2189,3705},{2224,3707},{2254,3709},{2284,3710},{2315,3712},
    {2345,3714},{2380,3715},{2410,3717},{2445,3719},{2480,3720},{2510,3722},{2545,3723},
    {2575,3725},{2610,3727},{2645,3730},{2680,3730},{2715,3732},{2745,3733},{2780,3735},
    {2810,3738},{2840,3738},{2870,3740},{2901,3742},{2936,3744},{2966,3746},{3001,3748},
    {3036,3750},{3066,3751},{3096,3754},{3126,3755},{3156,3756},{3191,3759},{3221,3761},
    {3251,3763},{3281,3765},{3311,3767},{3342,3769},{3376,3771},{3411,3773},{3442,3775},
    {3492,3778},{3527,3781},{3562,3783},{3597,3787},{3627,3787},{3657,3789},{3687,3793},
    {3717,3794},{3747,3795},{3782,3799},{3812,3801},{3843,3804},{3878,3806},{3913,3810},
    {3943,3811},{3978,3814},{4008,3816},{4043,3819},{4078,3823},{4108,3825},{4143,3827},
    {4173,3831},{4208,3835},{4238,3837},{4273,3840},{4304,3844},{4338,3846},{4369,3849},
    {4399,3852},{4429,3854},{4464,3857},{4499,3862},{4529,3865},{4559,3867},{4589,3870},
    {4619,3873},{4649,3876},{4679,3879},{4714,3883},{4744,3886},{4774,3888},{4809,3892},
    {4839,3895},{4874,3898},{4905,3901},{4940,3904},{4974,3908},{5005,3910},{5035,3914},
    {5065,3917},{5095,3918},{5125,3922},{5160,3926},{5195,3927},{5225,3929},{5255,3933},
    {5285,3937},{5315,3937},{5345,3940},{5380,3943},{5415,3946},{5445,3949},{5476,3951},
    {5510,3954},{5541,3958},{5576,3960},{5606,3960},{5636,3963},{5666,3966},{5696,3969},
    {5726,3969},{5761,3972},{5796,3976},{5826,3978},{5856,3979},{5886,3980},{5916,3982},
    {5946,3986},{5981,3988},{6012,3989},{6042,3993},{6077,3994},{6107,3997},{6137,3998},
    {6167,4000},{6202,4002},{6237,4006},{6267,4007},{6297,4010},{6327,4012},{6357,4011},
    {6387,4015},{6417,4017},{6447,4019},{6477,4021},{6508,4025},{6538,4027},{6568,4028},
    {6603,4031},{6633,4033},{6663,4037},{6698,4037},{6734,4040},{6765,4044},{6799,4046},
    {6829,4049},{6864,4052},{6894,4054},{6929,4057},{6959,4060},{6989,4063},{7019,4066},
    {7049,4069},{7080,4072},{7110,4074},{7140,4079},{7170,4081},{7205,4084},{7240,4087},
    {7270,4090},{7300,4093},{7330,4097},{7365,4098},{7395,4102},{7426,4106},{7456,4107},
    {7491,4111},{7526,4115},{7561,4118},{7591,4120},{7621,4122},{7656,4125},{7686,4127},
    {7716,4129},{7746,4132},{7776,4134},{7811,4136},{7841,4139},{7876,4141},{7911,4141},
    {7941,4144},{7976,4147},{8006,4149},{8042,4150},{8077,4152},{8107,4155},{8137,4156},
    {8167,4159},{8197,4159},{8232,4161},{8262,4163},{8292,4164},{8322,4165},{8358,4165},
    {8392,4166},{8422,4168},{8457,4169},{8492,4169},{8523,4170},{8557,4171},{8587,4171},
    {8618,4175},{8648,4176},{8678,4178},{8708,4180},{8738,4182},{8773,4185},{8803,4186},
    {8833,4188},{8868,4191},{8898,4191},{8928,4192},{8958,4193},{8993,4192},{9023,4193},
    {9054,4192},{9089,4192},{9119,4193},
};
constexpr uint32_t kChargeDone = 9144;
const Sample kEveningCharge[] = {
    {0,3675},{35,3683},{65,3689},{95,3694},{130,3698},{160,3703},{195,3707},{225,3710},{255,3712},{285,3714},
    {316,3717},{346,3720},{376,3721},{406,3723},{441,3726},{471,3728},{506,3731},{536,3733},{566,3736},
    {596,3737},{631,3740},{662,3743},{706,3746},{752,3748},{786,3750},{816,3753},{851,3755},{881,3757},
    {911,3760},{941,3761},{976,3764},{1212,3783},{1268,3788},{1298,3789},{1333,3793},{1368,3795},{1401,3798},
    {1465,3803},{1500,3807},{1590,3815},{1621,3817},{1747,3829},{1791,3834},{1821,3836},{1851,3838},
    {1886,3841},{1916,3844},{1947,3848},{1982,3851},{2017,3854},{2047,3857},{2077,3862},{2107,3864},
    {2137,3868},{2169,3870},{3466,3937},{3497,3955},{3527,3963},{3557,3969},{3587,3973},{3617,3977},
    {3647,3980},{3682,3984},{3712,3985},{3742,3988},{3772,3991},{3802,3993},{3832,3995},{3862,3999},
    {3892,3998},{3929,4002},{4121,4016},{4151,4017},{4186,4021},{4216,4023},{4247,4024},{4277,4027},
    {4307,4029},{4337,4032},{4368,4035},{4402,4037},{4432,4040},{4467,4043},{4846,4078},{4876,4079},
    {4911,4084},{4941,4087},{4971,4090},{5001,4093},{5031,4096},{5061,4099},{5096,4103},{5127,4104},
    {5162,4108},{5197,4112},{5227,4114},{5257,4117},{5292,4121},{5343,4124},{5374,4126},{5436,4131},
    {5470,4134},{5500,4137},{5530,4136},{5561,4138},{5596,4140},{5626,4144},{5656,4145},{5686,4147},
    {5716,4148},{5746,4151},{5776,4153},{5806,4154},{5841,4157},{5884,4160},{5915,4160},{5945,4162},
    {5975,4162},{6005,4163},{6035,4165},{6065,4167},{6100,4167},{6130,4168},{6160,4169},{6209,4170},
    {6241,4170},{6271,4171},{6302,4173},{6336,4175},{6366,4176},{6396,4178},{6427,4181},{6461,4183},
    {6492,4185},{6522,4188},{6557,4190},{6587,4191},{6617,4191},{6647,4192},{6677,4193},{6707,4192},
    {6742,4192},{6772,4193},{6807,4193},{6837,4193},
};
constexpr uint32_t kEveningDone = 6852;

// Percent charged at `t` before "done", for a charge that grows with time and
// takes kChargeDone from empty.
int charged_at(uint32_t t, uint32_t done) {
  return 100 - static_cast<int>((done - t) * 100 / kChargeDone);
}

struct Replay {
  int worst = 0, total = 0, n = 0;
  bool fell = false;
};

Replay replay_charge(const Sample* samples, size_t count, uint32_t done) {
  hg::BatteryPercent percent(hg::kAmoled175cCurve, hg::kAmoled175cChargeCurve);
  Replay r;
  int last = -1;
  uint32_t prev = 0;
  for (size_t i = 0; i < count; ++i) {
    const Sample& s = samples[i];
    const int reads = i && s.t - prev > 60 ? 12 : 6;  // 5 s reads; refill after a logging gap
    prev = s.t;
    int shown = 0;
    for (int k = 0; k < reads; ++k) shown = percent.update(s.mv, Charge::Charging, uint8_t(1));
    if (shown < last) r.fell = true;
    last = shown;
    const int err = std::abs(shown - charged_at(s.t, done));
    r.worst = std::max(r.worst, err);
    r.total += err;
    ++r.n;
  }
  return r;
}

}  // namespace

TEST("battery curve: interpolates between points and clamps at both ends") {
  CHECK_EQ(hg::percent_on_curve(hg::kAmoled175cCurve, 4200), uint8_t(100));
  CHECK_EQ(hg::percent_on_curve(hg::kAmoled175cCurve, 4100), uint8_t(100));
  CHECK_EQ(hg::percent_on_curve(hg::kAmoled175cCurve, 3704), uint8_t(49));
  CHECK_EQ(hg::percent_on_curve(hg::kAmoled175cCurve, 3729), uint8_t(52));  // halfway 3704..3754
  CHECK_EQ(hg::percent_on_curve(hg::kAmoled175cCurve, 3050), uint8_t(0));
  CHECK_EQ(hg::percent_on_curve(hg::kAmoled175cCurve, 2900), uint8_t(0));
}

TEST("battery curve: the 1.75C table falls monotonically from full to empty") {
  const hg::Curve c = hg::kAmoled175cCurve;
  for (size_t i = 1; i < c.size; ++i) {
    CHECK(c.points[i].mv < c.points[i - 1].mv);
    CHECK(c.points[i].percent < c.points[i - 1].percent);
  }
}

TEST("battery curve: replaying the measured night, the shown percent never rises and tracks the charge left") {
  hg::BatteryPercent percent(hg::kAmoled175cCurve);
  int last = 101, worst = 0;
  uint32_t prev = 0;
  for (const Sample& s : kNight) {
    // The device reads every 5 s; the log has a row every 30 s, except across two
    // stretches where it was offline (no rows) while still reading. After such a
    // gap, give the window the minute of readings it would have had.
    const int reads = s.t - prev > 60 ? 12 : 6;
    prev = s.t;
    int shown = 0;
    for (int i = 0; i < reads; ++i) shown = percent.update(s.mv, Charge::Discharging, std::nullopt);
    CHECK(shown <= last);
    last = shown;
    const int left = static_cast<int>((kShutdown - s.t) * 100 / kShutdown);
    worst = std::max(worst, std::abs(shown - left));
  }
  CHECK(worst <= 3);  // the gauge was off by up to 14 points over the same night
  CHECK(last <= 1);
}

TEST("battery curve: a lighter load lifting the voltage doesn't raise the percent") {
  hg::BatteryPercent percent(hg::kAmoled175cCurve);
  for (int i = 0; i < 12; ++i) percent.update(3704, Charge::Discharging, std::nullopt);
  CHECK_EQ(percent.update(3704, Charge::Discharging, std::nullopt), uint8_t(49));
  for (int i = 0; i < 12; ++i) percent.update(3754, Charge::Discharging, std::nullopt);  // screen off: +50 mV
  CHECK_EQ(percent.update(3754, Charge::Discharging, std::nullopt), uint8_t(49));
}

TEST("battery curve: one stray reading doesn't move it; a minute of lower readings does") {
  hg::BatteryPercent percent(hg::kAmoled175cCurve);
  for (int i = 0; i < 12; ++i) percent.update(3754, Charge::Discharging, std::nullopt);
  CHECK_EQ(percent.update(3600, Charge::Discharging, std::nullopt), uint8_t(55));  // a load spike
  for (int i = 0; i < 12; ++i) percent.update(3704, Charge::Discharging, std::nullopt);
  CHECK_EQ(percent.update(3704, Charge::Discharging, std::nullopt), uint8_t(49));
}

TEST("battery curve: charging shows the gauge, charged shows 100, unplugging starts afresh") {
  hg::BatteryPercent percent(hg::kAmoled175cCurve);
  for (int i = 0; i < 12; ++i) percent.update(3600, Charge::Discharging, std::nullopt);
  CHECK_EQ(percent.update(4150, Charge::Charging, uint8_t(40)), uint8_t(40));  // not the curve: voltage reads high
  CHECK_EQ(percent.update(4150, Charge::Charging, std::nullopt), uint8_t(100));  // no gauge: the curve, best effort
  CHECK_EQ(percent.update(4180, Charge::Done, uint8_t(97)), uint8_t(100));
  CHECK_EQ(percent.update(4120, Charge::Discharging, std::nullopt), uint8_t(100));  // not held at the old 32%
}

TEST("AXP2101: with a curve, the percent comes from it on battery and from the gauge on charge") {
  std::array<uint8_t, 256> regs{};
  regs[0x00] = 0x08;  // battery, no USB
  regs[0x01] = 0x45;  // discharging, not charging
  regs[0x18] = 0x0a;  // gauge on
  regs[0x30] = 0x03;  // voltage ADC on
  regs[0x34] = 0x0e;  // 3704 mV
  regs[0x35] = 0x78;
  regs[0xa4] = 36;    // the gauge's lower estimate
  hg::Axp2101 power(
      [&](uint8_t reg, uint8_t* out, size_t n) {
        for (size_t i = 0; i < n; ++i) out[i] = regs[reg + i];
        return true;
      },
      [&](uint8_t, uint8_t) { return true; });
  CHECK_EQ(power.read()->battery_percent, uint8_t(36));  // no curve: the gauge, as before
  power.use_curves(hg::kAmoled175cCurve);  // no charge curve: the gauge on charge
  CHECK_EQ(power.read()->battery_mv, uint16_t(3704));
  CHECK_EQ(power.read()->battery_percent, uint8_t(49));
  regs[0x00] = 0x28;  // USB in
  regs[0x01] = 0x22;  // charging, constant current
  CHECK_EQ(power.read()->battery_percent, uint8_t(36));
  regs[0x01] = 0x04;  // charge done
  CHECK_EQ(power.read()->battery_percent, uint8_t(100));
}

TEST("charge curve: replaying the charge from empty, it never falls and tracks the charge added") {
  const Replay r = replay_charge(kChargeFromEmpty, sizeof(kChargeFromEmpty) / sizeof(kChargeFromEmpty[0]), kChargeDone);
  CHECK(!r.fell);
  CHECK(r.worst <= 2);
}

TEST("charge curve: on a charge it wasn't fitted to, it stays close; the gauge read up to 18 low") {
  const Replay r = replay_charge(kEveningCharge, sizeof(kEveningCharge) / sizeof(kEveningCharge[0]), kEveningDone);
  CHECK(!r.fell);
  CHECK(r.total <= 2 * r.n);  // within 2 points on average
  CHECK(r.worst <= 7);        // the first minutes after plugging in, before the voltage settles
}

TEST("charge curve: plugging in never drops below what battery showed; it holds 99 until done") {
  hg::BatteryPercent percent(hg::kAmoled175cCurve, hg::kAmoled175cChargeCurve);
  for (int i = 0; i < 12; ++i) percent.update(3704, Charge::Discharging, std::nullopt);
  CHECK_EQ(percent.update(3704, Charge::Discharging, std::nullopt), uint8_t(49));
  CHECK_EQ(percent.update(3740, Charge::Charging, std::nullopt), uint8_t(49));  // charge curve says 31
  for (int i = 0; i < 12; ++i) percent.update(3904, Charge::Charging, std::nullopt);
  CHECK_EQ(percent.update(3904, Charge::Charging, std::nullopt), uint8_t(54));
  for (int i = 0; i < 12; ++i) percent.update(4205, Charge::Charging, std::nullopt);
  CHECK_EQ(percent.update(4205, Charge::Charging, std::nullopt), uint8_t(99));
  CHECK_EQ(percent.update(4190, Charge::Done, std::nullopt), uint8_t(100));
}

TEST("charge curve: the 1.75C charge table rises monotonically from empty to 99") {
  const hg::Curve c = hg::kAmoled175cChargeCurve;
  CHECK_EQ(c.points[0].percent, uint8_t(99));
  for (size_t i = 1; i < c.size; ++i) {
    CHECK(c.points[i].mv < c.points[i - 1].mv);
    CHECK(c.points[i].percent < c.points[i - 1].percent);
  }
}

TEST("AXP2101: the charge current limit decodes both step sizes, and reserved values are unknown") {
  uint8_t icc = 0x09;
  hg::Axp2101 power(
      [&](uint8_t reg, uint8_t* out, size_t) {
        *out = reg == 0x62 ? icc : 0;
        return true;
      },
      [&](uint8_t, uint8_t) { return false; });
  CHECK(power.charge_current_ma() == std::optional<uint16_t>(300));  // the datasheet default (Nox reads 200)
  icc = 0x04;
  CHECK(power.charge_current_ma() == std::optional<uint16_t>(100));
  icc = 0x15;
  CHECK(power.charge_current_ma() == std::optional<uint16_t>(1500));
  icc = 0x16;
  CHECK(!power.charge_current_ma().has_value());
}
