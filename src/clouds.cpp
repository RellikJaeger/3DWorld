// 3D World - Cloud Generation and Drawing Code
// by Frank Gennari
// 3/10/02
#include "3DWorld.h"
#include "physics_objects.h"
#include "shaders.h"
#include "gl_ext_arb.h"


bool     const USE_CLOUD_FBO    = 1;
unsigned const CLOUD_GEN_TEX_SZ = 1024;


vector2d cloud_wind_pos(0.0, 0.0);
cloud_manager_t cloud_manager;

extern bool have_sun, no_sun_lpos_update;
extern int window_width, window_height, cloud_model, display_mode, xoff, yoff;
extern float CLOUD_CEILING, atmosphere, sun_rot, fticks, water_plane_z, zmin, zmax;
extern vector3d wind;


void draw_part_cloud(vector<particle_cloud> const &pc, colorRGBA const color, bool zoomed);


struct cloud_t {
	unsigned b, e;
	point p;
	float r;
	cloud_t(unsigned b_=0, unsigned e_=0, point const &p_=all_zeros, float r_=0.0)
		: b(b_), e(e_), p(p_), r(r_) {}
};


float get_xy_cloud_scale() {return ((world_mode == WMODE_INF_TERRAIN) ? 4.0 : 1.0);}


void cloud_manager_t::create_clouds() { // 3D cloud puffs

	float const xy_scale(get_xy_cloud_scale()), xsz(X_SCENE_SIZE*xy_scale), ysz(Y_SCENE_SIZE*xy_scale);
	if (!empty() && xy_scale == last_xy_scale) return; // keep the old clouds
	last_xy_scale = xy_scale;
	clear();
	free_textures();
	srand(123);
	unsigned const NCLOUDS = 10;
	unsigned const NPARTS  = 1000;

	for (unsigned c = 0; c < NCLOUDS; ++c) {
		point const center(4.0*xsz*signed_rand_float(), 4.0*ysz*signed_rand_float(),
						   (ztop + CLOUD_CEILING + Z_SCENE_SIZE*rand_uniform(0.25, 0.75)));
		point const bounds(xsz*rand_uniform(1.0, 2.0), ysz*rand_uniform(1.0, 2.0),
						   Z_SCENE_SIZE*rand_uniform(0.4, 0.8));
		unsigned const nparts(rand()%(NPARTS/2) + NPARTS/2);
		size_t const ix(size());
		resize(ix + nparts);

		for (unsigned p = 0; p < nparts; ++p) {
			point pos(signed_rand_vector_spherical(1.0));

			for (unsigned i = 0; i < 3; ++i) {
				pos[i] *= bounds[i];
			}
			if (pos.z < 0.0) pos.z *= 0.5; // compressed on the bottom
			pos += center;
			float const radius(0.045*(xsz + ysz)*rand_uniform(0.5, 1.0));
			float const density(rand_uniform(0.05, 0.12));
			(*this)[ix + p].gen(pos, WHITE, zero_vector, radius, density, 0.0, 0.0, -((int)c+2), 0, 0, 1, 1); // no lighting
		}
	}
}


void cloud_manager_t::update_lighting() {

	RESET_TIME;
	point const sun_pos(get_sun_pos());
	bool const calc_sun_light(have_sun && light_factor > 0.4);
	unsigned const num_clouds((unsigned)size());
	int last_src(0);
	vector<cloud_t> clouds;

	if (calc_sun_light) {
		for (unsigned i = 0; i < num_clouds; ++i) {
			particle_cloud &c((*this)[i]);

			if (i == 0 || c.source != last_src) {
				last_src = c.source;
				if (i > 0) clouds.back().e = i; // end the last cloud
				clouds.push_back(cloud_t(i));   // begin a new cloud
			}
		}
		clouds.back().e = num_clouds; // end the last cloud

		for (unsigned i = 0; i < clouds.size(); ++i) {
			cloud_t &c(clouds[i]);
			for (unsigned j = c.b; j < c.e; ++j) c.p += (*this)[j].pos;
			c.p /= (c.e - c.b);
			for (unsigned j = c.b; j < c.e; ++j) c.r = max(c.r, (p2p_dist(c.p, (*this)[j].pos) + (*this)[j].radius));
		}
	}
	for (unsigned i = 0; i < num_clouds; ++i) {
		particle_cloud &pc((*this)[i]);
		float light(0.25); // night time sky

		if (calc_sun_light) {
			vector3d const v1(sun_pos - pc.pos);
			float const dist_sq(v1.mag_sq());
			vector3d const v1n(v1/dist_sq);
			light = 1.0; // start off fully lit

			for (unsigned p = 0; p < clouds.size(); ++p) {
				cloud_t &c(clouds[p]);
				float t; // unused

				if (sphere_test_comp(sun_pos, c.p, v1, c.r*c.r, t)) {
					for (unsigned j = c.b; j < c.e; ++j) {
						particle_cloud const &c2((*this)[j]);
						vector3d const v2(sun_pos, c2.pos);
						if (v2.mag_sq() > dist_sq) continue; // further from the sun
						float const dotp(dot_product(v1, v2));
						float const dsq((dotp > dist_sq) ? p2p_dist_sq(v1, v2) : (v2 - v1n*dotp).mag_sq());
						if (dsq > c2.radius*c2.radius) continue; // no intersection
						float const alpha(2.0*c2.base_color.alpha*c2.density*((c2.radius - sqrt(dsq))/c2.radius));
						light *= 1.0 - CLIP_TO_01(alpha);
					}
				}
			}
			if (light_factor < 0.6) {
				float const blend(5.0*(light_factor - 0.4));
				light = light*blend + 0.25*(1.0 - blend);
			}
		}
		pc.darkness   = 1.0 - 2.0*light;
		pc.base_color = WHITE;
		apply_red_sky(pc.base_color);
	}
	PRINT_TIME("Cloud Lighting");
}


cube_t cloud_manager_t::get_bcube() const {

	cube_t bcube;

	for (unsigned i = 0; i < size(); ++i) {
		point const &pos((*this)[i].pos);
		float const radius((*this)[i].radius);

		if (i == 0) {
			bcube = cube_t(pos, pos);
			bcube.expand_by(radius);
		}
		else {
			bcube.union_with_sphere(pos, radius);
		}
	}
	return bcube;
}


float cloud_manager_t::get_max_xy_extent() const {

	cube_t const bcube(get_bcube());
	return(max(max(-bcube.d[0][0], bcube.d[0][1]), max(-bcube.d[1][0], bcube.d[1][1])));
}


bool cloud_manager_t::create_texture(bool force_recreate) {

	RESET_TIME;
	unsigned const xsize(USE_CLOUD_FBO ? CLOUD_GEN_TEX_SZ : min(CLOUD_GEN_TEX_SZ, (unsigned)window_width));
	unsigned const ysize(USE_CLOUD_FBO ? CLOUD_GEN_TEX_SZ : min(CLOUD_GEN_TEX_SZ, (unsigned)window_height));

	if (txsize != xsize || tysize != ysize) {
		free_textures();
		txsize = xsize;
		tysize = ysize;
	}
	if (cloud_tid && !force_recreate) return 0; // nothing to do
	
	if (!cloud_tid) {
		setup_texture(cloud_tid, GL_MODULATE, 0, 0, 0);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, xsize, ysize, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
	}
	assert(glIsTexture(cloud_tid));
	check_gl_error(800);
	if (USE_CLOUD_FBO) enable_fbo(fbo_id, cloud_tid, 0);
	check_gl_error(801);

	glViewport(0, 0, xsize, ysize);
	glClearColor(1.0, 1.0, 1.0, 1.0); // white
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();

	// setup projection matrix
	cube_t const bcube(get_bcube());
	float const cloud_bot(bcube.d[2][0]), cloud_top(bcube.d[2][1]), cloud_xy(get_max_xy_extent());
	float const scene_xy(get_xy_cloud_scale()*max(X_SCENE_SIZE, Y_SCENE_SIZE)), angle(atan2(cloud_xy, cloud_bot)), z1(min(zbottom, czmin));
	frustum_z = z1 - scene_xy*(cloud_bot - z1)/(cloud_xy - scene_xy);
	//pos_dir_up const pdu(get_pt_cube_frustum_pdu(get_camera_pos(), bcube, 1));
	//pos_dir_up const pdu(all_zeros, plus_z, plus_x, tanf(angle)*SQRT2, sinf(angle), NEAR_CLIP, FAR_CLIP, 1.0);
	//gluPerspective(2.0*angle/TO_RADIANS, 1.0, cloud_bot-frustum_z, cloud_top+(cloud_top - cloud_bot)-frustum_z);
	gluPerspective(2.0*angle/TO_RADIANS, 1.0, NEAR_CLIP, FAR_CLIP);
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glLoadIdentity();
	vector3d const up_dir(plus_y);
	point const origin(0.0, 0.0, frustum_z), center(0.0, 0.0, cloud_bot);
	gluLookAt(origin.x, origin.y, origin.z, center.x, center.y, center.z, up_dir.x, up_dir.y, up_dir.z);

	set_red_only(1);
	point const orig_cpos(camera_pos);
	bool const was_valid(camera_pdu.valid);
	camera_pdu.valid = 0; // disable view frustum culling
	camera_pos = origin;
	draw_part_cloud(*this, WHITE, 1); // draw clouds
	camera_pos = orig_cpos;
	camera_pdu.valid = was_valid;
	set_red_only(0);

	if (!USE_CLOUD_FBO) { // render clouds to texture
		glBindTexture(GL_TEXTURE_2D, cloud_tid);
		glReadBuffer(GL_BACK);
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, xsize, ysize); // copy the frame buffer to the bound texture
	}

	// reset state
	glMatrixMode(GL_PROJECTION);
	glPopMatrix();
	glMatrixMode(GL_MODELVIEW);
	glPopMatrix();
	if (USE_CLOUD_FBO) disable_fbo();
	glViewport(0, 0, window_width, window_height);
	if (!USE_CLOUD_FBO) glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

	check_gl_error(802);
	PRINT_TIME("Cloud Texture Gen");
	return 1;
}


void free_cloud_textures() {

	cloud_manager.free_textures();
}


void cloud_manager_t::free_textures() {

	free_texture(cloud_tid);
	free_fbo(fbo_id);
}


//http://www.gamedev.net/reference/articles/article2273.asp
void cloud_manager_t::draw() {

	if (atmosphere < 0.01) return; // no atmosphere
	create_clouds();
	if (empty()) return;
	//glFinish(); // testing
	RESET_TIME;
	glDisable(GL_DEPTH_TEST);

	// WRITE: wind moves clouds

	// light source code
	static bool had_sun(0);
	static float last_sun_rot(0.0);
	bool const need_update(!no_sun_lpos_update && (sun_rot != last_sun_rot || have_sun != had_sun));

	if (need_update) {
		last_sun_rot = sun_rot;
		had_sun      = have_sun;
		update_lighting();
	}
	if (cloud_model == 0) { // faster billboard texture mode
		create_texture(need_update);
		enable_flares(get_cloud_color(), 1); // texture will be overriden
		assert(cloud_tid);
		bind_2d_texture(cloud_tid);

		shader_t s;
		s.set_vert_shader("no_lighting_tex_coord");
		s.set_frag_shader("cloud_billboard");
		s.begin_shader();
		s.add_uniform_int("tex0", 0);

		glBegin(GL_QUADS);
		point const camera(get_camera_pos());
		cube_t const bcube(get_bcube());
		float const cloud_bot(bcube.d[2][0]), cloud_top(bcube.d[2][1]), cloud_xy(get_max_xy_extent());
		float const xy_exp((cloud_top - frustum_z)/(cloud_bot - frustum_z));
		
		for (unsigned d = 0; d < 2; ++d) { // render the bottom face of bcube
			for (unsigned e = 0; e < 2; ++e) {
				glTexCoord2f(float(d^e^1), float(d));
				point(xy_exp*((d^e) ? cloud_xy : -cloud_xy)+camera.x, xy_exp*(d ? cloud_xy : -cloud_xy)+camera.y, cloud_top).do_glVertex();
			}
		}
		glEnd();
		s.end_shader();
		disable_flares();
	}
	else {
		draw_part_cloud(*this, get_cloud_color(), 1);
	}
	glEnable(GL_DEPTH_TEST);
	//glFinish(); // testing
	//PRINT_TIME("Clouds");
}


void draw_puffy_clouds(int order) {

	if (cloud_manager.is_inited() && (get_camera_pos().z > cloud_manager.get_z_plane()) != order) return;

	if (atmosphere < 0.01) {
		cloud_manager.clear();
	}
	else if (display_mode & 0x40) { // key 7
		cloud_manager.draw();
	}
}


void draw_cloud_vert(float x, float y, float z1, float z2, float r) {
	float const xx(x - get_camera_pos().x), yy(y - get_camera_pos().y);
	glVertex3f(x, y, (z1 + (z2 - z1)*cos(PI_TWO*min(1.0f, sqrt(xx*xx + yy*yy)/r))));
}


void set_cloud_uniforms(shader_t &s, unsigned tu_id) {

	select_multitex(NOISE_TEX, tu_id, 0);
	s.add_uniform_int("cloud_noise_tex", tu_id);
	set_multitex(0);
	point const camera(get_camera_pos()), world_pos(camera + vector3d((xoff2-xoff)*DX_VAL, (yoff2-yoff)*DY_VAL, 0.0));
	vector3d const offset(-camera + 0.5*world_pos); // relative cloud velocity is half the camera velocity
	s.add_uniform_vector3d("offset", offset);
	s.add_uniform_vector2d("dxy", cloud_wind_pos);
}


void draw_cloud_plane(bool reflection_pass) {

	float const size(FAR_CLIP), rval(0.94*size); // extends to at least the far clipping plane
	float const z1(zmin), z2(get_camera_pos().z + max(zmax, CLOUD_CEILING));
	cloud_wind_pos.x += fticks*wind.x;
	cloud_wind_pos.y += fticks*wind.y;
	shader_t s;
	glDepthMask(GL_FALSE);

	// draw a plane at zmin to properly blend the fog
	if (!reflection_pass) {
		s.set_prefix("#define USE_QUADRATIC_FOG", 1); // FS
		s.set_vert_shader("fog_only");
		s.set_frag_shader("linear_fog.part+fog_only");
		s.begin_shader();
		s.setup_fog_scale();
		BLACK.do_glColor();
		draw_z_plane(-size, -size, size, size, zmin, 4, 4);
		s.end_shader();
	}

	// draw clouds
	s.set_prefix("#define USE_QUADRATIC_FOG", 1); // FS
	s.set_vert_shader("clouds");
	s.set_frag_shader("linear_fog.part+perlin_clouds.part+clouds");
	s.begin_shader();
	s.setup_fog_scale();
	set_cloud_uniforms(s, 0);
	enable_blend();
	get_cloud_color().do_glColor();
	glBegin(GL_QUADS);
	unsigned const NUM_DIV = 32;
	float const dxy(2*size/(NUM_DIV-1));
	float yval(-size);

	for (unsigned i = 0; i < NUM_DIV; ++i) {
		float xval(-size);

		for (unsigned j = 0; j < NUM_DIV; ++j) {
			draw_cloud_vert( xval,       yval,      z1, z2, rval);
			draw_cloud_vert((xval+dxy),  yval,      z1, z2, rval);
			draw_cloud_vert((xval+dxy), (yval+dxy), z1, z2, rval);
			draw_cloud_vert( xval,      (yval+dxy), z1, z2, rval);
			xval += dxy;
		}
		yval += dxy;
	}
	glEnd();
	s.end_shader();
	disable_blend();
	glDepthMask(GL_TRUE);
}


