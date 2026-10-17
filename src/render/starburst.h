// The starburst's sums (shaders/composite.frag's starburst repeats them): how many lines of streaks an iris draws,
// and its two settings packed into one push constant (the composite's are all used).
#pragma once

namespace starburst {

// Lines through each bright spot (each a streak both ways): an iris of n blades draws n streaks when n is even (they
// pair up across the spot: n / 2 lines), 2n when odd (n lines)
int lines(int blades);
// The blades (4..9) and the strength (0..1) in one float: blades + strength * 0.99, and back
float pack(int blades, float strength);
void unpack(float packed, int& blades, float& strength);

}  // namespace starburst
