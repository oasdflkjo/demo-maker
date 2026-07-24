#version 330 core

out vec4 out_color;

uniform vec2 u_resolution;
uniform float u_time;
uniform float u_beat;
uniform float u_beat_phase;
uniform float u_bar_phase;
uniform float u_pulse;

// The effect editor discovers these annotations and generates controls.
// @param u_star_count "Star count" 84 16 160 1
uniform float u_star_count;
// @param u_camera_speed "Camera speed" 0.115 0.01 0.4 0.005
uniform float u_camera_speed;
// @param u_view_distance "View distance" 1.0 0.4 2.0 0.01
uniform float u_view_distance;
// @param u_near_plane "Near plane" 0.055 0.01 0.2 0.005
uniform float u_near_plane;
// @param u_camera_wander "Camera wander" 1.0 0.0 3.0 0.05
uniform float u_camera_wander;
// @param u_camera_curve "Camera curve speed" 0.32 0.0 1.2 0.01
uniform float u_camera_curve;
// @param u_star_size "Star size" 1.0 0.25 3.0 0.05
uniform float u_star_size;
// @param u_streak_length "Streak length" 0.035 0.005 0.16 0.005
uniform float u_streak_length;
// @param u_streak_strength "Streak strength" 0.55 0.0 2.0 0.05
uniform float u_streak_strength;
// @param u_far_fade "Far fade start" 0.72 0.2 0.95 0.01
uniform float u_far_fade;
// @param u_exposure "Exposure" 1.0 0.1 3.0 0.05
uniform float u_exposure;
// @param u_beat_reactivity "Beat reactivity" 0.055 0.0 0.8 0.01
uniform float u_beat_reactivity;

const int MAX_STAR_COUNT = 160;

float hash11(float value) {
    return fract(sin(value * 127.1) * 43758.5453123);
}

vec3 hash31(float value) {
    return fract(sin(value * vec3(127.1, 311.7, 74.7))
               * vec3(43758.5453, 22578.1459, 19642.3490));
}

float distance_to_segment(vec2 point, vec2 start, vec2 end) {
    vec2 segment = end - start;
    float length_squared = max(dot(segment, segment), 0.0000001);
    float along = clamp(dot(point - start, segment) / length_squared, 0.0, 1.0);
    return length(point - (start + segment * along));
}

vec2 camera_path(float time) {
    return u_camera_wander * vec2(
        sin(time * 0.31) * 0.18 + sin(time * 0.071) * 0.09,
        cos(time * 0.23) * 0.12 + sin(time * 0.053) * 0.07
    );
}

void main() {
    // Screen coordinates use pixel height as their unit so stars remain round.
    vec2 screen = (2.0 * gl_FragCoord.xy - u_resolution.xy)
                / min(u_resolution.x, u_resolution.y);
    float pixel = 1.0 / min(u_resolution.x, u_resolution.y);

    vec3 color = vec3(0.0015, 0.0025, 0.006);
    float travel = u_time * u_camera_speed;
    vec2 camera_now = camera_path(u_time * u_camera_curve);
    vec2 camera_before =
        camera_path((u_time - u_streak_length) * u_camera_curve);

    for (int index = 0; index < MAX_STAR_COUNT; ++index) {
        if (float(index) >= u_star_count) {
            break;
        }
        float seed = float(index) + 11.73;
        vec3 random = hash31(seed);

        // Each star wraps from the near plane back to the far plane. Together,
        // the independently offset stars form a bounded 3D volume.
        float normalized_depth = fract(random.z - travel);
        float depth =
            mix(u_near_plane, u_view_distance, normalized_depth);
        float previous_depth =
            min(depth + u_camera_speed * u_streak_length, u_view_distance);

        vec2 world = (random.xy - 0.5) * vec2(3.8, 2.35);
        vec2 position = (world - camera_now) / depth;
        vec2 previous_position = (world - camera_before) / previous_depth;

        vec2 point_delta = abs(screen - position);
        float square_distance = max(point_delta.x, point_delta.y);
        float streak_distance =
            distance_to_segment(screen, previous_position, position);

        float near_amount = 1.0 - normalized_depth;
        float half_size_pixels =
            mix(0.45, 1.4, near_amount * near_amount) * u_star_size;
        float half_size = half_size_pixels * pixel * 2.0;
        float antialias = pixel * 0.8;

        // Axis-aligned screen-space squares keep the stars visibly pixel-like.
        float point =
            1.0 - smoothstep(half_size, half_size + antialias, square_distance);

        // Streaks appear only on nearby, fast-moving stars.
        float streak_radius = half_size * 0.32;
        float streak =
            1.0 - smoothstep(streak_radius,
                             streak_radius + antialias, streak_distance);
        streak *= smoothstep(0.40, 0.96, near_amount) * u_streak_strength;

        // The volume is visibly finite: new stars emerge softly at the view
        // distance and disappear before crossing the camera.
        float far_fade =
            1.0 - smoothstep(u_far_fade, 1.0, normalized_depth);
        float near_fade = smoothstep(0.015, 0.09, normalized_depth);
        float visibility = far_fade * near_fade;

        float magnitude = mix(0.28, 1.0, hash11(seed * 5.91));
        float twinkle = 0.88 + 0.12 * sin(u_time * (1.0 + random.x * 2.0)
                                      + seed);
        color += vec3(1.0) * (point + streak) * visibility
               * magnitude * twinkle;
    }

    // A restrained musical pulse changes exposure without moving the stars
    // off their sample-accurate visual clock.
    color *= u_exposure * (1.0 + u_pulse * u_beat_reactivity);

    float vignette = 1.0 - smoothstep(0.72, 1.55, length(screen));
    color *= mix(0.72, 1.0, vignette);
    color = pow(max(color, 0.0), vec3(0.82));

    out_color = vec4(color, 1.0);
}
