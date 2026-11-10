// Stock area "cut" outlines in entity coordinates. Socket yaw points outward.
// Non-convex outlines are split into triangles without filling their notches.
constexpr TrenchPoint cut_straight[]{
    {-101.707f, 19.3524f}, {-101.4f, -22.0729f}, {100.918f, -21.4006f},
    {100.918f, 19.413f},
};
constexpr TrenchPoint cut_straight_short[]{
    {-58.7802f, 19.3524f}, {-58.4729f, -18.8477f}, {55.593f, -18.1754f},
    {55.593f, 19.413f},
};
constexpr TrenchPoint cut_turn[]{
    {52.3248f, 21.167f}, {-49.0003f, 21.5548f}, {-56.7431f, -11.2958f},
    {-29.2196f, -19.7158f}, {1.23216f, -23.3073f}, {29.3415f, -19.8756f},
    {59.275f, -13.0999f},
};
constexpr TrenchPoint cut_turn2[]{
    {38.8421f, 25.8256f}, {-48.6363f, 33.0612f}, {-61.1807f, 2.61482f},
    {-37.1094f, -7.78236f}, {-8.13561f, -12.7528f}, {21.3264f, -11.696f},
    {44.9134f, -5.1952f},
};
constexpr TrenchPoint cut_turn3[]{
    {47.585f, 21.5049f}, {-44.6573f, 21.3384f}, {-59.5866f, -8.08935f},
    {-34.8815f, -19.7158f}, {1.23216f, -25.902f}, {40.4004f, -19.8756f},
    {61.7316f, -7.5373f},
};
constexpr TrenchPoint cut_turn_90[]{
    {15.9438f, -47.3686f}, {47.5107f, -16.1969f}, {47.5107f, 16.0754f},
    {16.8792f, 16.0754f}, {0.509165f, 11.7101f}, {-12.2751f, 1.26444f},
    {-17.1081f, -16.041f}, {-17.1081f, -47.2127f},
};
constexpr TrenchPoint cut_intersection[]{
    {-14.2588f, -34.1947f}, {15.6469f, -34.1947f}, {49.3946f, -2.16401f},
    {49.3946f, 28.0379f}, {-47.9123f, 28.0379f}, {-47.9123f, -2.16401f},
};
constexpr TrenchPoint cut_end[]{
    {-82.4767f, 21.3567f}, {-93.2771f, 2.23117f}, {-85.8563f, -14.7265f},
    {-19.5591f, -23.4355f}, {46.2927f, -17.1806f}, {46.2927f, 17.2567f},
};
constexpr TrenchPoint cut_exit[]{
    {-38.8867f, 19.2718f}, {-38.5794f, -20.0783f}, {30.9551f, -18.8973f},
    {30.9551f, 19.7001f},
};
constexpr TrenchPoint cut_bridge[]{
    {-61.9277f, 17.0658f}, {-61.9277f, -19.6776f}, {0.431444f, -27.8244f},
    {62.9364f, -20.6574f}, {62.9364f, 16.6027f},
};
constexpr TrenchPoint cut_fire_position1[]{
    {-42.6325f, -18.3476f}, {-10.135f, -34.5148f}, {-2.13312f, -36.4744f},
    {7.01191f, -34.1881f}, {39.836f, -17.3678f}, {39.836f, 19.0172f},
    {-42.3059f, 19.0172f},
};
constexpr TrenchPoint cut_fire_position2[]{
    {28.7753f, 17.0568f}, {-4.50302f, 17.0568f}, {-21.16f, 12.8109f},
    {-24.4261f, -0.416762f}, {-19.8536f, -13.6444f}, {-3.19659f, -18.3632f},
    {28.811f, -18.3632f},
};
constexpr TrenchPoint cut_mg_nest[]{
    {0.914009f, 29.4621f}, {-68.9431f, 17.9506f}, {-68.6759f, -16.7938f},
    {-2.24771f, -47.7151f}, {63.9885f, -16.2798f}, {64.219f, 18.8747f},
};
constexpr TrenchPoint cut_emplacement[]{
    {-12.1042f, 46.9284f}, {-36.5241f, 52.516f}, {-59.0387f, 52.2037f},
    {-92.4183f, 29.5389f}, {-99.2711f, -12.7512f}, {-81.2796f, -38.9737f},
    {-55.7366f, -52.2127f}, {-19.1051f, -50.1142f}, {58.1607f, -16.3504f},
    {57.924f, 16.5545f},
};
constexpr TrenchPoint cut_pillbox[]{
    {-15.0f, -25.0f}, {25.0f, -25.0f}, {80.0f, -15.0f},
    {80.0f, 15.0f}, {25.0f, 25.0f}, {-15.0f, 25.0f},
};
constexpr TrenchPoint cut_depot[]{
    {-82.7097f, 19.3995f}, {-82.5391f, -16.8226f}, {-26.8085f, -38.4233f},
    {6.18794f, -42.0252f}, {79.6973f, -17.2058f}, {79.6973f, 20.5298f},
};

constexpr TrenchModule trenchModules[]{
    {straightTrench, "it_trench_straight_preview", cut_straight, ARRAYSIZE(cut_straight),
     2, {{{-100.0f, 0.0f}, 180.0f}, {{100.0f, 0.0f}, 0.0f}}, nullptr, 0, 1},
    {"it_trench_straight_short", "it_trench_straight_short_preview", cut_straight_short, ARRAYSIZE(cut_straight_short),
     2, {{{-56.517f, -0.201f}, 180.0f}, {{53.631f, -0.201f}, 0.0f}}},
    {"it_trench_turn", "it_trench_turn_preview", cut_turn, ARRAYSIZE(cut_turn),
     2, {{{-50.795f, 4.552f}, 170.0f}, {{54.055f, 3.412f}, 10.0f}}, nullptr, 0, 1},
    {"it_trench_turn2", "it_trench_turn2_preview", cut_turn2, ARRAYSIZE(cut_turn2),
     2, {{{-52.87f, 17.758f}, 160.0f}, {{39.735f, 9.571f}, 10.0f}}, nullptr, 0, 1},
    {"it_trench_turn3", "it_trench_turn3_preview", cut_turn3, ARRAYSIZE(cut_turn3),
     2, {{{-51.417f, 7.438f}, 150.0f}, {{53.56f, 8.038f}, 30.0f}}, nullptr, 0, 1},
    {"it_trench_turn_90", "it_trench_turn_90_preview", cut_turn_90, ARRAYSIZE(cut_turn_90),
     2, {{{-0.201f, -50.001f}, -90.0f}, {{50.011f, 0.184f}, 0.0f}}},
    {"it_trench_intersection", "it_trench_intersection_preview", cut_intersection, ARRAYSIZE(cut_intersection),
     3, {{{-49.259f, 13.229f}, 180.0f}, {{50.741f, 13.129f}, 0.0f}, {{1.316f, -35.265f}, -90.0f}}},
    {"it_trench_end", "it_trench_end_preview", cut_end, ARRAYSIZE(cut_end),
     1, {{{45.107f, -0.201f}, 0.0f}}},
    {"it_trench_exit", "it_trench_exit_preview", cut_exit, ARRAYSIZE(cut_exit),
     1, {{{23.551f, -0.201f}, 0.0f, 8.0f}}},
    {"it_trench_bridge", "it_trench_bridge_preview", cut_bridge, ARRAYSIZE(cut_bridge),
     2, {{{-61.53f, -0.209f}, 180.0f}, {{62.533f, -0.209f}, 0.0f}}, nullptr, 0, 1},
    {"it_trench_fire_position1", "it_trench_fire_position1_preview", cut_fire_position1, ARRAYSIZE(cut_fire_position1),
     2, {{{-41.5253f, 0.0f}, 180.0f}, {{38.6248f, 0.0f}, 0.0f}}},
    {"it_trench_fire_position2", "it_trench_fire_position2_preview", cut_fire_position2, ARRAYSIZE(cut_fire_position2),
     1, {{{27.5415f, 0.0f}, 0.0f}}},
    {"it_trench_mg_nest", "it_trench_mg_nest_preview", cut_mg_nest, ARRAYSIZE(cut_mg_nest),
     2, {{{-67.11f, -0.201f}, 180.0f}, {{62.739f, -0.201f}, 0.0f}}, nullptr, 0, 1},
    {"it_trench_emplacement", "it_trench_emplacement_preview", cut_emplacement, ARRAYSIZE(cut_emplacement),
     1, {{{56.956f, -0.601f}, 0.0f}}},
    {"it_trench_pillbox", "it_trench_pillbox_preview", cut_pillbox, ARRAYSIZE(cut_pillbox),
     1, {{{75.651f, 0.0f}, 0.0f}}},
    {"it_trench_depot", "it_trench_depot_preview", cut_depot, ARRAYSIZE(cut_depot),
     2, {{{-80.459f, -0.201f}, 180.0f}, {{78.528f, -0.201f}, 0.0f}}, nullptr, 0, 1},
};
