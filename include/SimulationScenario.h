#pragma once
#include <cmath>
#include <cstdint>
#include <limits>

namespace ffheater {
namespace simulation {
struct Point { uint16_t minute; float room; float duct; };
struct Sample { float room; float duct; bool deliver; };
static constexpr Point A[] = {
 {0,0,0},{2,1,40},{5,2,45},{10,3,50},{15,4,55},
 {20,5,55},{25,6,55},{30,7,55},{35,8,55},{40,9,55},{45,10,55},{75,10,10}
};
static constexpr Point B[] = {
 {0,0,0},{2,1,40},{5,2,45},{10,3,50},{15,4,55},
 {20,5,55},{25,6,55},{30,7,55},{45,10,55}
};
static constexpr Point C[] = {
 {0,0,0},{2,1,40},{5,2,45},{10,3,50},{15,4,55},
 {20,5,55},{25,6,55},{30,7,55},{35,8,55},{40,9,55},
 {45,10,55},{50,10,30},{55,8,10},{60,5,5},{62,5,40},{75,9,55}
};
static constexpr Point FAIL[] = {{0,0,0},{20,0,0}};
static constexpr Point LOST[] = {{0,0,0},{2,1,40},{15,3,55}};
static constexpr Point SENSORS[] = {{0,0,0},{2,1,40},{20,3,55}};
// Fault cases start after ignition confirmation; null sensor values become NaN.
inline Sample sample(uint8_t scenario, uint32_t second) {
 const Point* points = A; size_t count = sizeof(A)/sizeof(A[0]);
 switch (scenario) {
  case 2: points=B;count=sizeof(B)/sizeof(B[0]);break;
  case 3: points=C;count=sizeof(C)/sizeof(C[0]);break;
  case 4: points=FAIL;count=sizeof(FAIL)/sizeof(FAIL[0]);break;
  case 5: points=LOST;count=sizeof(LOST)/sizeof(LOST[0]);break;
  case 6: points=SENSORS;count=sizeof(SENSORS)/sizeof(SENSORS[0]);break;
 }
 const Point* lo=&points[0]; const Point* hi=&points[count-1];
 for (size_t i=1;i<count;++i) if (second<=uint32_t(points[i].minute)*60) {
  lo=&points[i-1];hi=&points[i];break;
 }
 const uint32_t a=uint32_t(lo->minute)*60,b=uint32_t(hi->minute)*60;
 const float ratio=b>a ? float(int64_t(second)-a)/float(b-a) : 0.0f;
 float room=lo->room+(hi->room-lo->room)*ratio;
 float duct=lo->duct+(hi->duct-lo->duct)*ratio;
 if (scenario==5 && second>=180) return {room,duct,false};
 if (scenario==6 && second>=180) room=duct=std::numeric_limits<float>::quiet_NaN();
 return {room,duct,true};
}
}
}
