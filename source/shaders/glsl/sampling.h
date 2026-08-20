#define RND_NORMALIZER ( 1.0 / float( 0xFFFFFFFFu ) );

// NOTE(marco): as implemented in https://www.reedbeta.com/blog/quick-and-easy-gpu-random-numbers-in-d3d11/
uint wang_hash( uint seed ) {
    seed = (seed ^ 61) ^ (seed >> 16);
    seed *= 9;
    seed = seed ^ (seed >> 4);
    seed *= 0x27d4eb2d;
    seed = seed ^ (seed >> 15);
    return seed;
}

uint get_uniform_random_value( uint state ) {
    // Xorshift algorithm from George Marsaglia's paper
    state ^= ( state << 13 );
    state ^= ( state >> 17 );
    state ^= ( state << 5 );
    return state;
}

// NOTE(marco): as implemented in https://www.reedbeta.com/blog/hash-functions-for-gpu-rendering/
uint rand_pcg( inout uint rng_state ) {
    uint state = rng_state;
    rng_state = rng_state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float rand( inout uint rng_state ) {
    return float( rand_pcg( rng_state ) ) * RND_NORMALIZER;
}

uvec2 pcg2d( uvec2 v )
{
    v = v * 1664525u + 1013904223u;

    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;

    v = v ^ ( v >> 16u );

    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;

    v = v ^ (v >> 16u);

    return v;
}

// NOTE(marco): reference: https://jcgt.org/published/0009/03/02/ - Hash Functions for GPU Rendering
uint seed(uvec2 p) {
    return 19u * p.x + 47u * p.y + 101u;
}

// NOTE(marco): taken from https://jcgt.org/published/0007/04/01/ - Sampling the GGX Distribution of Visible Normals
// Input Ve: view direction
// Input alpha_x, alpha_y: roughness parameters
// Input U1, U2: uniform random numbers
// Output Ne: normal sampled with PDF D_Ve(Ne) = G1(Ve) * max(0, dot(Ve, Ne)) * D(Ne) / Ve.z
vec3 sampleGGXVNDF(vec3 Ve, float alpha_x, float alpha_y, float U1, float U2)
{
    // Section 3.2: transforming the view direction to the hemisphere configuration
    vec3 Vh = normalize(vec3(alpha_x * Ve.x, alpha_y * Ve.y, Ve.z));
    // Section 4.1: orthonormal basis (with special case if cross product is zero)
    float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    vec3 T1 = lensq > 0 ? vec3(-Vh.y, Vh.x, 0) * inversesqrt(lensq) : vec3(1,0,0);
    vec3 T2 = cross(Vh, T1);
    // Section 4.2: parameterization of the projected area
    float r = sqrt(U1);
    float phi = 2.0 * PI * U2;
    float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    float s = 0.5 * (1.0 + Vh.z);
    t2 = (1.0 - s)*sqrt(1.0 - t1*t1) + s*t2;
    // Section 4.3: reprojection onto hemisphere
    vec3 Nh = t1*T1 + t2*T2 + sqrt(max(0.0, 1.0 - t1*t1 - t2*t2))*Vh;
    // Section 3.4: transforming the normal back to the ellipsoid configuration
    vec3 Ne = normalize(vec3(alpha_x * Nh.x, alpha_y * Nh.y, max(0.0, Nh.z)));
    return Ne;
}

float PdfCosineHemisphere(vec3 n, vec3 f)
{
    float cosTheta = max(dot(n, f), 0.0);
    return cosTheta * INV_PI;
}

float D_GGX_Alpha( float NoH, float alpha )
{
    float a2 = alpha * alpha;
    float d = NoH * NoH * (a2 - 1.0) + 1.0;

    return a2 / max( PI * d * d, 1e-20 );
}

float SmithG1_GGX( float NoV, float alpha )
{
    if ( NoV <= 0.0 )
        return 0.0;

    float a2 = alpha * alpha;
    float root = sqrt( a2 + (1.0 - a2) * NoV * NoV );

    return ( 2.0 * NoV ) / max( NoV + root, 1e-20 );
}


float PdfGGXVNDF(vec3 n, vec3 Ve, vec3 f, float alpha)
{
    float NoV = max(dot(n, Ve), 0.0);
    float NoL = max(dot(n, f), 0.0);

    if (NoV <= 0.0 || NoL <= 0.0)
        return 0.0;

    vec3 h = normalize(Ve + f);

    float NoH = max(dot(n, h), 0.0);
    float VoH = max(dot(Ve, h), 0.0);

    if (NoH <= 0.0 || VoH <= 0.0)
        return 0.0;

    float D = D_GGX_Alpha( NoH, alpha );
    float G1 = SmithG1_GGX( NoV, alpha );

    // VNDF half-vector PDF:
    float pdf_h = D * G1 * VoH / NoV;

    // Reflection Jacobian: h -> wi
    float pdf_wi = pdf_h / (4.0 * VoH);

    return pdf_wi;
}

struct BrdfSample {
    vec3 wi;
    float pdf;
};

mat3 make_tangent_frame( vec3 n ) {
    vec3 up = abs( n.z ) < 0.999 ? vec3( 0.0, 0.0, 1.0 ) : vec3( 1.0, 0.0, 0.0 );
    vec3 t = normalize( cross( up, n ) );
    vec3 b = cross( n, t );
    return mat3( t, b, n ); // local -> world
}

vec3 world_to_local( mat3 frame, vec3 v ) {
    return transpose( frame ) * v;
}

BrdfSample sampleVNDFOrDiffuse(vec3 Ve, vec3 albedo, vec3 n, float metalness, float roughness_x, float roughness_y, inout uint rng_state, float U1, float U2) {
    mat3 frame = make_tangent_frame(n);

    vec3 wo_local = world_to_local( frame, Ve );

    BrdfSample brdfSample;
    brdfSample.wi = vec3( 0.0 );
    brdfSample.pdf = 0.0;

    if (wo_local.z <= 0.0)
        return brdfSample;

    vec3 F0 = mix( vec3( 0.04 ), albedo, metalness );
    // Approximate relative strength of each lobe.
    float diffuseWeight  = luminance(albedo) * (1.0 - metalness);
    float specularWeight = luminance(F0);

    float sum = diffuseWeight + specularWeight;

    if (sum <= 0.0)
    {
        diffuseWeight = 1.0;
        specularWeight = 0.0;
    }
    else
    {
        diffuseWeight  /= sum;
        specularWeight /= sum;
    }

    float alpha_x = max(roughness_x * roughness_x, 0.001);
    float alpha_y = max(roughness_y * roughness_y, 0.001);

    vec3 wi_local;
    if ( rand( rng_state ) <= specularWeight ) {
        vec3 h_local = sampleGGXVNDF( wo_local, alpha_x, alpha_y, U1, U2 );
        wi_local = reflect( -wo_local, h_local );
    } else {
        // Sample a cosine-weighted hemisphere
        float r = sqrt(U1);
        float phi = 2.0 * PI * U2;
        float x = r * cos(phi);
        float y = r * sin(phi);
        float z = sqrt(max(0.0, 1.0 - x*x - y*y));
        wi_local = vec3(x, y, z);
    }

    if (wi_local.z <= 0.0) wi_local.z *= -1.0;

    vec3 wi = normalize( frame * wi_local );

    // Compute total probability of the sample.
    float pdfDiffuse = PdfCosineHemisphere(n, wi);
    float pdfSpec    = PdfGGXVNDF(n, Ve, wi, alpha_x);

    brdfSample.wi = wi;
    brdfSample.pdf = diffuseWeight * pdfDiffuse + specularWeight * pdfSpec;

    return brdfSample;
}

// Adapted from https://www.pbr-book.org/4ed/Sampling_Algorithms/Sampling_Multidimensional_Functions#UniformlySamplingHemispheresandSpheres
BrdfSample sample_unform_diffuse( vec2 rnd ) {
    float z = rnd.x;
    float r = sqrt( max( 1 - z*z, 0 ) );
    float phi = 2 * PI * rnd.y;

    BrdfSample brdfSample;
    brdfSample.wi = vec3( r * cos(phi), r * sin(phi), z );
    brdfSample.pdf = 1 / ( 2 * PI );

    return brdfSample;
}
