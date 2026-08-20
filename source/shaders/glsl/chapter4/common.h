
#define PI 3.1415926538
#define INVALID_TEXTURE_INDEX 65535

uint DrawFlags_AlphaMask = 1 << 0;
uint DrawFlags_HasTangents = 1 << 6;

vec3 diffuse_brdf( vec3 color ) {
    return ( 1.0 / PI ) * color;
}

vec3 specular_brdf( float alpha_squared, vec3 N, vec3 L, vec3 H, vec3 V ) {
    float NdotH = clamp( dot( N, H ), 0, 1.0 );
    float NdotL = clamp( dot( N, L ), 0, 1.0 );
    float NdotV = clamp( dot( N, V ), 0, 1.0 );
    float HdotL = clamp( dot( H, L ), 0, 1.0 );
    float HdotV = clamp( dot( H, V ), 0, 1.0 );
    float ggx = ( alpha_squared * heaviside( NdotH ) ) /
        ( PI * (
            pow( ( ( NdotH * NdotH ) * ( alpha_squared - 1.0 ) + 1 ), 2 )
        ) );

    float visiblity = ( heaviside( HdotL ) / ( abs( NdotL ) + sqrt( alpha_squared + ( 1 - alpha_squared ) * ( NdotL * NdotL ) ) ) ) *
                      ( heaviside( HdotV ) / ( abs( NdotV ) + sqrt( alpha_squared + ( 1 - alpha_squared ) * ( NdotV * NdotV ) ) ) );

    return vec3( visiblity * ggx );
}
