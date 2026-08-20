
#if defined(VERTEX)

layout(location=0) in vec3 position;

void main() {

    mat4 model = mesh_draws[gl_InstanceIndex].model;
    gl_Position = view_projection * model * vec4(position, 1.0);
}

#endif // VERTEX
