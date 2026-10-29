#pragma once
#include "core/Math.h"
#include <cstdint>

namespace alone {

enum class VehicleKind : uint8_t { Bike, Car };

// A bike or a car that has been moved off its parking spot. Parked cars and
// bikes you haven't touched are just props on the map; trying a car door
// (or getting on a bike) turns it into one of these.
struct Vehicle {
    int id = 0;
    VehicleKind kind = VehicleKind::Car;
    Vec2 pos;                // centre
    float heading = 0;       // radians, the way the front points
    float speed = 0;         // m/s along the heading (negative: reversing)
    int variant = 0;         // colour / body style
    bool keyInIgnition = false;
    bool hotwired = false;
    bool windowSmashed = false;
    bool running = false;    // engine on
    float fuelL = 0;         // petrol in the tank
    float tankL = 50;        // how much it holds
    float damage = 0;        // 0..1, 1 = wrecked
    bool searched = false;   // glovebox
    bool canStart() const { return kind == VehicleKind::Bike || keyInIgnition || hotwired; }
    float length() const { return kind == VehicleKind::Car ? 4.3f : 1.7f; }
    float halfWidth() const { return kind == VehicleKind::Car ? 0.88f : 0.3f; }
    Vec2 forward() const;
};

} // namespace alone
