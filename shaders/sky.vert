#version 450
// The sky: a full-screen triangle at the far plane, drawn after the opaque objects (only where nothing is)

layout(location = 0) out vec2 vNdc;

void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vNdc = p * 2.0 - 1.0;
    gl_Position = vec4(vNdc, 1.0, 1.0);
}
