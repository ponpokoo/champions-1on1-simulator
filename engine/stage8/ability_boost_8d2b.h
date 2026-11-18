// Generated from review_8d2b.json by gen_ability_boost_8d2b.py.
enum {
    S8Q_BIGPECKS=12,
    S8Q_CLEARBODY=17,
    S8Q_COMPETITIVE=19,
    S8Q_COMPOUNDEYES=20,
    S8Q_CONTRARY=21,
    S8Q_DAMP=27,
    S8Q_DEFIANT=28,
    S8Q_GUARDDOG=59,
    S8Q_HUSTLE=68,
    S8Q_HYPERCUTTER=70,
    S8Q_ILLUMINATE=72,
    S8Q_KEENEYE=83,
    S8Q_MAGICBOUNCE=94,
    S8Q_MIRRORARMOR=104,
    S8Q_NOGUARD=112,
    S8Q_OPPORTUNIST=114,
    S8Q_SANDVEIL=152,
    S8Q_SNOWCLOAK=166,
    S8Q_TANGLEDFEET=195,
    S8Q_WHITESMOKE=214,
    S8Q_SIMPLE=216,
};
typedef struct { int ability, accuracy, condition, guard_stat, change, react_stat, breakable; } S8BoostAbility;
static const S8BoostAbility S8_BOOST_ABILITIES[]={
    {S8Q_BIGPECKS,0,0,1,0,-1,1},
    {S8Q_CLEARBODY,0,0,7,0,-1,1},
    {S8Q_COMPETITIVE,0,0,-1,0,2,0},
    {S8Q_COMPOUNDEYES,5325,0,-1,0,-1,0},
    {S8Q_CONTRARY,0,0,-1,-1,-1,1},
    {S8Q_DAMP,0,0,-1,0,-1,1},
    {S8Q_DEFIANT,0,0,-1,0,0,0},
    {S8Q_GUARDDOG,0,0,-1,0,-1,1},
    {S8Q_HUSTLE,3277,4,-1,0,-1,0},
    {S8Q_HYPERCUTTER,0,0,0,0,-1,1},
    {S8Q_ILLUMINATE,0,0,5,0,-1,1},
    {S8Q_KEENEYE,0,0,5,0,-1,1},
    {S8Q_MAGICBOUNCE,0,0,-1,0,-1,1},
    {S8Q_MIRRORARMOR,0,0,-1,0,-1,1},
    {S8Q_NOGUARD,0,0,-1,0,-1,0},
    {S8Q_OPPORTUNIST,0,0,-1,0,-1,0},
    {S8Q_SANDVEIL,3277,1,-1,0,-1,1},
    {S8Q_SNOWCLOAK,3277,2,-1,0,-1,1},
    {S8Q_TANGLEDFEET,2048,3,-1,0,-1,1},
    {S8Q_WHITESMOKE,0,0,7,0,-1,1},
    {S8Q_SIMPLE,0,0,-1,2,-1,1},
};
#define S8_BOOST_HANDLER_ROWS \
    {S8Q_BIGPECKS,S8AE_TRY_BOOST,0,0,0,s8_boost_callback}, \
    {S8Q_COMPETITIVE,S8AE_AFTER_EACH_BOOST,0,0,0,s8_boost_callback}, \
    {S8Q_COMPOUNDEYES,S8AE_SOURCE_MODIFY_ACCURACY,0,-1,0,s8_boost_callback}, \
    {S8Q_CONTRARY,S8AE_CHANGE_BOOST,0,0,0,s8_boost_callback}, \
    {S8Q_DAMP,S8AE_TRY_MOVE,0,0,0,s8_boost_callback}, \
    {S8Q_DAMP,S8AE_ANY_DAMAGE,0,0,0,s8_boost_callback}, \
    {S8Q_DEFIANT,S8AE_AFTER_EACH_BOOST,0,0,0,s8_boost_callback}, \
    {S8Q_GUARDDOG,S8AE_TRY_BOOST,0,2,0,s8_boost_callback}, \
    {S8Q_GUARDDOG,S8AE_DRAG_OUT,0,1,0,s8_boost_callback}, \
    {S8Q_HUSTLE,S8AE_SOURCE_MODIFY_ACCURACY,0,-1,0,s8_boost_callback}, \
    {S8Q_HUSTLE,S8AE_ATK,0,5,0,s8_boost_callback}, \
    {S8Q_HYPERCUTTER,S8AE_TRY_BOOST,0,0,0,s8_boost_callback}, \
    {S8Q_ILLUMINATE,S8AE_TRY_BOOST,0,0,0,s8_boost_callback}, \
    {S8Q_ILLUMINATE,S8AE_MODIFY_MOVE,0,0,0,s8_boost_callback}, \
    {S8Q_KEENEYE,S8AE_TRY_BOOST,0,0,0,s8_boost_callback}, \
    {S8Q_KEENEYE,S8AE_MODIFY_MOVE,0,0,0,s8_boost_callback}, \
    {S8Q_MAGICBOUNCE,S8AE_TRY_HIT,0,1,0,s8_boost_callback}, \
    {S8Q_MAGICBOUNCE,S8AE_TRY_HIT_SIDE,0,0,0,s8_boost_callback}, \
    {S8Q_MIRRORARMOR,S8AE_TRY_BOOST,0,0,0,s8_boost_callback}, \
    {S8Q_NOGUARD,S8AE_ALWAYS_ACCURACY,0,0,0,s8_boost_callback}, \
    {S8Q_NOGUARD,S8AE_INVULNERABILITY,0,1,0,s8_boost_callback}, \
    {S8Q_OPPORTUNIST,S8AE_FOE_AFTER_BOOST,0,0,0,s8_boost_callback}, \
    {S8Q_OPPORTUNIST,S8AE_AFTER_SWITCH_IN,0,-3,0,s8_boost_callback}, \
    {S8Q_OPPORTUNIST,S8AE_AFTER_MEGA,0,0,0,s8_boost_callback}, \
    {S8Q_OPPORTUNIST,S8AE_AFTER_TERA,0,0,0,s8_boost_callback}, \
    {S8Q_OPPORTUNIST,S8AE_AFTER_MOVE,0,0,0,s8_boost_callback}, \
    {S8Q_OPPORTUNIST,S8AE_RESIDUAL,29,0,0,s8_boost_callback}, \
    {S8Q_OPPORTUNIST,S8AE_END,0,0,0,s8_boost_callback}, \
    {S8Q_SANDVEIL,S8AE_MODIFY_ACCURACY,0,-1,0,s8_boost_callback}, \
    {S8Q_SANDVEIL,S8AE_IMMUNITY,0,0,0,s8_boost_callback}, \
    {S8Q_SNOWCLOAK,S8AE_MODIFY_ACCURACY,0,-1,0,s8_boost_callback}, \
    {S8Q_SNOWCLOAK,S8AE_IMMUNITY,0,0,0,s8_boost_callback}, \
    {S8Q_TANGLEDFEET,S8AE_MODIFY_ACCURACY,0,-1,0,s8_boost_callback}, \
    {S8Q_WHITESMOKE,S8AE_TRY_BOOST,0,0,0,s8_boost_callback}, \
    {S8Q_SIMPLE,S8AE_CHANGE_BOOST,0,0,0,s8_boost_callback},
static const S8BoostAbility *s8_boost_ability(int id) {
    for(unsigned i=0;i<sizeof(S8_BOOST_ABILITIES)/sizeof(*S8_BOOST_ABILITIES);i++) if(S8_BOOST_ABILITIES[i].ability==id) return &S8_BOOST_ABILITIES[i];
    return NULL;
}
static int s8_boost_implemented(int id) { return s8_boost_ability(id)!=NULL; }
// bigpecks 7b7690a63444da0eae16739ca7af1b7e144a52fb539dc87849859baeb9723903
// clearbody 216afd6116f63d242bbe7bd0e641bb327917e44d369624bd8fe932c74469b717
// competitive aa7ae0d8b126c35bf6075e7a20012e851e12531c7017ce2acbbeeef580076c93
// compoundeyes 3582b95dba642fd3549eba7d359b97a095e96dfed15f152ad8e1db831ba5e5ed
// contrary b52312db127089f14a96ca0a7e3f982f47cc76d27d191dfd886ae7efe0638d45
// damp 1a7f32a8415b52525e947e6369d995047214aa119ace919b8a09710f5b2f6705
// defiant edf32f04b3f7195c6a0cab7a7169a0cc9d993545a26a620cf52737587127af19
// guarddog c259865042ae9cb227f18bed20d18dfdc7a2b9064c54cafc008f05644a9da487
// hustle 9f34ca70665fc5625ca7af4ea6a1ffa933132c1303e5a5689b7754cc8aa8a4d1
// hypercutter 676ad6be294db1e66aaabe8b344522b0cf2f25aca53aeec5babc5d5cb33d2f4e
// illuminate 9ad483cf7c6f5a8a753b15ba0140e058d96ff55d1e28bd058c6deae1465f894a
// keeneye 9678129182c44f364ff887a4dd8d14ec9edd3993d57ab96e3843afe8f1896fe6
// magicbounce 95d9b71188b74912e97b219dcca6d2d57bb654f0f4363767844c170a2fb818fb
// mirrorarmor f9c7d3b050a8f674f371b7c7abef65f5eb9c513e0969c2c711ca058621c74945
// noguard 9481c82918ea131332c47633614839aa05d7bbd1da65533939efb12915f2085a
// opportunist 99b27ea7e9a150df0b481d4d08b56e171eb6696d1935abac72937b5003ddca1c
// sandveil e747bbd9415c099a70a9931f830e5b98c2cf39838e7f67a4d27815bc570b133b
// snowcloak 8c23b97be40b286c00e49ee346f4dbebc35e1f3d7ff9092139191b2685c3a6e8
// tangledfeet e14ced0705b71a76cccabee9e5ce5027ae7f4661552c6a80f5bec54524d29b74
// whitesmoke dcf92abe2c32d3f7ec09e8125ca11e55c2913b0720dd3335e6de92132bdab2be
// simple eacb66bf64715f4fba29be56c5aaf5cedef40e26c234aaef5c9383b8f656de96
