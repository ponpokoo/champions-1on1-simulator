// Generated from review_8d3b.json; pinned Champions definitions.
enum {
    S8V3_ARMORTAIL=6,
    S8V3_CHLOROPHYLL=16,
    S8V3_EARLYBIRD=34,
    S8V3_GALEWINGS=53,
    S8V3_HEALER=62,
    S8V3_HYDRATION=69,
    S8V3_ICEBODY=71,
    S8V3_MOODY=106,
    S8V3_POISONHEAL=124,
    S8V3_PRANKSTER=127,
    S8V3_QUEENLYMAJESTY=134,
    S8V3_QUICKDRAW=135,
    S8V3_QUICKFEET=136,
    S8V3_RAINDISH=137,
    S8V3_SANDFORCE=148,
    S8V3_SANDRUSH=149,
    S8V3_SHEDSKIN=159,
    S8V3_SLUSHRUSH=164,
    S8V3_SOLARPOWER=168,
    S8V3_SPEEDBOOST=171,
    S8V3_STALL=174,
    S8V3_SURGESURFER=189,
    S8V3_SWIFTSWIM=192,
};
static int s8_weather_implemented(int id) {
    switch(id) {
    case S8V3_ARMORTAIL:
    case S8V3_CHLOROPHYLL:
    case S8V3_EARLYBIRD:
    case S8V3_GALEWINGS:
    case S8V3_HEALER:
    case S8V3_HYDRATION:
    case S8V3_ICEBODY:
    case S8V3_MOODY:
    case S8V3_POISONHEAL:
    case S8V3_PRANKSTER:
    case S8V3_QUEENLYMAJESTY:
    case S8V3_QUICKDRAW:
    case S8V3_QUICKFEET:
    case S8V3_RAINDISH:
    case S8V3_SANDFORCE:
    case S8V3_SANDRUSH:
    case S8V3_SHEDSKIN:
    case S8V3_SLUSHRUSH:
    case S8V3_SOLARPOWER:
    case S8V3_SPEEDBOOST:
    case S8V3_STALL:
    case S8V3_SURGESURFER:
    case S8V3_SWIFTSWIM:
        return 1;
    default:return 0;
    }
}
#define S8_WEATHER_HANDLER_ROWS \
    {S8V3_ARMORTAIL,S8AE_FOE_TRY_MOVE,0,0,0,s8_weather_callback}, \
    {S8V3_CHLOROPHYLL,S8AE_SPEED,0,0,0,s8_weather_callback}, \
    {S8V3_GALEWINGS,S8AE_PRIORITY,0,0,0,s8_weather_callback}, \
    {S8V3_HEALER,S8AE_RESIDUAL,5,0,3,s8_weather_callback}, \
    {S8V3_HYDRATION,S8AE_RESIDUAL,5,0,3,s8_weather_callback}, \
    {S8V3_ICEBODY,S8AE_IMMUNITY,0,0,0,s8_weather_callback}, \
    {S8V3_ICEBODY,S8AE_WEATHER_TICK,0,0,0,s8_weather_callback}, \
    {S8V3_MOODY,S8AE_RESIDUAL,28,0,2,s8_weather_callback}, \
    {S8V3_POISONHEAL,S8AE_INDIRECT,0,1,0,s8_weather_callback}, \
    {S8V3_PRANKSTER,S8AE_PRIORITY,0,0,0,s8_weather_callback}, \
    {S8V3_QUEENLYMAJESTY,S8AE_FOE_TRY_MOVE,0,0,0,s8_weather_callback}, \
    {S8V3_QUICKDRAW,S8AE_FRACTIONAL_PRIORITY,0,-1,0,s8_weather_callback}, \
    {S8V3_QUICKFEET,S8AE_SPEED,0,0,0,s8_weather_callback}, \
    {S8V3_RAINDISH,S8AE_WEATHER_TICK,0,0,0,s8_weather_callback}, \
    {S8V3_SANDFORCE,S8AE_BASE_POWER,0,21,0,s8_weather_callback}, \
    {S8V3_SANDFORCE,S8AE_IMMUNITY,0,0,0,s8_weather_callback}, \
    {S8V3_SANDRUSH,S8AE_SPEED,0,0,0,s8_weather_callback}, \
    {S8V3_SANDRUSH,S8AE_IMMUNITY,0,0,0,s8_weather_callback}, \
    {S8V3_SHEDSKIN,S8AE_RESIDUAL,5,0,3,s8_weather_callback}, \
    {S8V3_SLUSHRUSH,S8AE_SPEED,0,0,0,s8_weather_callback}, \
    {S8V3_SOLARPOWER,S8AE_SPA,0,5,0,s8_weather_callback}, \
    {S8V3_SOLARPOWER,S8AE_WEATHER_TICK,0,0,0,s8_weather_callback}, \
    {S8V3_SPEEDBOOST,S8AE_RESIDUAL,28,0,2,s8_weather_callback}, \
    {S8V3_STALL,S8AE_FRACTIONAL_PRIORITY,0,0,0,s8_weather_callback}, \
    {S8V3_SURGESURFER,S8AE_SPEED,0,0,0,s8_weather_callback}, \
    {S8V3_SWIFTSWIM,S8AE_SPEED,0,0,0,s8_weather_callback},
