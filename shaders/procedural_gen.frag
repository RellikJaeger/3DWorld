uniform float min_alpha = 0.0;

// clipped eye position, clipped vertex position
varying vec3 vpos, normal; // world space
varying vec3 eye_norm;

void main()
{
	vec3 norm_normal = normalize(normal);
	vec4 texel;

	if (use_noise_tex) {
		texel = get_texture_val(norm_normal, vpos);
	}
	else {
		texel = lookup_triplanar_texture(vpos, norm_normal, tex0, tex0, tex0) * color0;
	}
	float alpha = gl_Color.a;
	vec3 lit_color = gl_Color.rgb; // base color (with some lighting)
	add_indir_lighting(lit_color);
	vec4 epos = gl_ModelViewMatrix * vec4(vpos, 1.0);
	
	// directional light sources with no attenuation (Note: could add other lights later)
	if (enable_light0)  lit_color += add_light_comp_pos_smap_light0(normalize(eye_norm), epos).rgb;
	if (enable_light1)  lit_color += add_light_comp_pos_smap_light1(normalize(eye_norm), epos).rgb;
	if (enable_dlights) lit_color += add_dlights(vpos, norm_normal, gl_ModelViewMatrixInverse[3].xyz, gl_FrontMaterial.diffuse.rgb); // dynamic lighting
	vec4 color = vec4((texel.rgb * lit_color), (texel.a * alpha));
#ifndef NO_ALPHA_TEST
	if (color.a <= min_alpha) discard;
#endif
#ifndef NO_FOG
	color = apply_fog_epos(color, epos); // apply standard fog
#endif
	gl_FragColor = color;
}
