// 3D World - Building vs. Player/AI Interaction Logic
// by Frank Gennari 1/14/21

#include "3DWorld.h"
#include "function_registry.h"
#include "buildings.h"
#include "openal_wrap.h"

extern float fticks, CAMERA_RADIUS;
extern double tfticks;

bldg_obj_type_t bldg_obj_types[NUM_TYPES];


// lights

float get_radius_for_room_light(room_object_t const &obj);

bool building_t::toggle_room_light(point const &closest_to) { // Note: called by the player; closest_to is in building space, not camera space
	if (!has_room_geom()) return 0; // error?
	vector<room_object_t> &objs(interior->room_geom->objs);
	auto objs_end(objs.begin() + interior->room_geom->stairs_start); // skip stairs and elevators
	point query_pt(closest_to);
	if (is_rotated()) {do_xy_rotate_inv(bcube.get_cube_center(), query_pt);}
	int const room_id(get_room_containing_pt(query_pt));
	if (room_id < 0) return 0; // closest_to is not contained in a room of this building
	room_t const &room(get_room(room_id));
	point light_pos;
	float closest_dist_sq(0.0);
	unsigned closest_light(0);

	for (auto i = objs.begin(); i != objs_end; ++i) {
		if (!i->is_light_type() || i->room_id != room_id) continue; // not a light, or the wrong room
		if (!room.is_sec_bldg && get_floor_for_zval(i->z1()) != get_floor_for_zval(closest_to.z)) continue; // wrong floor (skip garages and sheds)
		point center(i->get_cube_center());
		if (is_rotated()) {do_xy_rotate(bcube.get_cube_center(), center);}
		float const dist_sq(p2p_dist_sq(closest_to, center));
		if (closest_dist_sq == 0.0 || dist_sq < closest_dist_sq) {closest_dist_sq = dist_sq; closest_light = (i - objs.begin()); light_pos = center;}
	} // for i
	assert(closest_light < objs.size());
	room_object_t &light(objs[closest_light]);
	bool updated(0);

	for (auto i = objs.begin(); i != objs_end; ++i) { // toggle all lights on this floor of this room
		if (i->is_light_type() && i->room_id == room_id && i->z1() == light.z1()) {
			i->toggle_lit_state(); // Note: doesn't update indir lighting
			if (i->type == TYPE_LAMP) continue; // lamps don't affect room object ambient lighting, and don't require regenerating the vertex data, so skip the step below
			set_obj_lit_state_to(room_id, light.z2(), i->is_lit()); // update object lighting flags as well
			updated = 1;
		}
	} // for i
	if (updated) {interior->room_geom->clear_and_recreate_lights();} // recreate light geom with correct emissive properties
	point const sound_pos(get_camera_pos() + (light_pos - closest_to)); // Note: computed relative to closest_to so that this works for either camera or building coord space
	gen_sound(SOUND_CLICK, sound_pos);
	return 1;
}

bool building_t::set_room_light_state_to(room_t const &room, float zval, bool make_on) { // called by AI people
	if (!has_room_geom()) return 0; // error?
	if (room.is_hallway)  return 0; // don't toggle lights for hallways, which can have more than one light
	vector<room_object_t> &objs(interior->room_geom->objs);
	auto objs_end(objs.begin() + interior->room_geom->stairs_start); // skip stairs and elevators
	float const window_vspacing(get_window_vspace());
	bool updated(0);

	for (auto i = objs.begin(); i != objs_end; ++i) {
		if (i->type != TYPE_LIGHT) continue; // not a light (excludes lamps)
		if (i->z1() < zval || i->z1() > (zval + window_vspacing) || !room.contains_cube_xy(*i)) continue; // light is on the wrong floor or in the wrong room
		if (i->is_lit() != make_on) {i->toggle_lit_state(); updated = 1;} // Note: doesn't update indir lighting or room light value
	} // for i
	if (updated) {interior->room_geom->clear_and_recreate_lights();} // recreate light geom with correct emissive properties
	return updated;
}

void building_t::set_obj_lit_state_to(unsigned room_id, float light_z2, bool lit_state) {
	assert(has_room_geom());
	float const light_intensity(get_room(room_id).light_intensity);
	vector<room_object_t> &objs(interior->room_geom->objs);
	auto objs_end(objs.begin() + interior->room_geom->stairs_start); // skip stairs and elevators
	float const obj_zmin(light_z2 - get_window_vspace()); // get_floor_thickness()?
	bool was_updated(0);

	for (auto i = objs.begin(); i != objs_end; ++i) {
		if (i->room_id != room_id || i->z1() < obj_zmin || i->z1() > light_z2) continue; // wrong room or floor
		if (i->is_obj_model_type()) continue; // light_amt currently does not apply to 3D models; should it?

		if (i->type == TYPE_STAIR || i->type == TYPE_STAIR_WALL || i->type == TYPE_ELEVATOR || i->type == TYPE_LIGHT || i->type == TYPE_BLOCKER ||
			i->type == TYPE_COLLIDER || i->type == TYPE_SIGN || i->type == TYPE_WALL_TRIM || i->type == TYPE_RAILING || i->type == TYPE_BLINDS)
		{
			continue; // not a type that uses light_amt
		}
		else if (i->type == TYPE_WINDOW) {
			if (lit_state) {i->flags |= RO_FLAG_LIT;} else {i->flags &= ~RO_FLAG_LIT;}
		}
		else {
			if (lit_state) {i->light_amt += light_intensity;} else {i->light_amt = max((i->light_amt - light_intensity), 0.0f);} // shouldn't be negative, but clamp to 0 just in case
		}
		was_updated = 1;
	} // for i
	if (was_updated) {interior->room_geom->clear_materials();} // need to recreate them
}

// doors

int building_t::find_ext_door_close_to_point(tquad_with_ix_t &door, point const &pos, float dist) const {
	point query_pt(pos);
	if (is_rotated()) {do_xy_rotate_inv(bcube.get_cube_center(), query_pt);}
	int const room_id(get_room_containing_pt(query_pt));
	cube_t room_exp;

	if (room_id >= 0) { // pos is inside a room
		room_exp = get_room(room_id);
		room_exp.expand_by(get_wall_thickness()); // make sure it contains the door
	}
	// Note: returns the first exterior door found, assuming there can be at most one within dist of pos
	for (auto d = doors.begin(); d != doors.end(); ++d) {
		cube_t c(d->get_bcube());
		if (room_id >= 0 && !room_exp.contains_cube(c)) continue; // door not in the same room as pos - there is likely a wall between them
		c.expand_by_xy(dist);
		if (c.contains_pt(query_pt)) {door = *d; return (d - doors.begin());}
	} // for d
	return -1; // not found
}

void building_t::register_open_ext_door_state(int door_ix) {
	bool const is_open(door_ix >= 0), was_open(open_door_ix >= 0);
	if (is_open == was_open) return; // no state change
	unsigned const dix(is_open ? (unsigned)door_ix : (unsigned)open_door_ix);
	assert(dix < doors.size());
	point const sound_pos(doors[dix].get_bcube().get_cube_center() + get_camera_coord_space_xlate()); // convert to camera space
	gen_sound((is_open ? (unsigned)SOUND_DOOR_OPEN : (unsigned)SOUND_DOOR_CLOSE), sound_pos);
	open_door_ix = door_ix;
}

bool check_door_dir(point const &closest_to, vector3d const &in_dir, cube_t const &door, point const &center) { // Note: only zvals of door are used, no rotate required
	if (in_dir == zero_vector) return 1; // direction filter specified
	point vis_pt(center.x, center.y, closest_to.z); // use query point zval
	max_eq(vis_pt.z, door.z1()); // clamp visibility test point to z-range of door to allow the player to open the door even looking at the top or bottom of it
	min_eq(vis_pt.z, door.z2());
	return (dot_product(in_dir, (vis_pt - closest_to).get_norm()) > 0.5); // door is not in the correct direction, skip
}

bool building_t::toggle_door_state_closest_to(point const &closest_to, vector3d const &in_dir) { // called for the player
	if (!interior) return 0; // error?
	float closest_dist_sq(0.0);
	unsigned door_ix(0);
	bool is_closet(0);

	for (auto i = interior->doors.begin(); i != interior->doors.end(); ++i) {
		if (i->z1() > closest_to.z || i->z2() < closest_to.z) continue; // wrong floor, skip
		point center(i->get_cube_center());
		if (is_rotated()) {do_xy_rotate(bcube.get_cube_center(), center);}
		if (!check_door_dir(closest_to, in_dir, *i, center)) continue; // door is not in the correct direction, skip
		float const dist_sq(p2p_dist_sq(closest_to, center));
		if (closest_dist_sq == 0.0 || dist_sq < closest_dist_sq) {closest_dist_sq = dist_sq; door_ix = (i - interior->doors.begin());}
	} // for i
	if (is_house && interior->room_geom) { // check for closet doors; only houses have closets
		vector<room_object_t> &objs(interior->room_geom->objs);

		for (auto i = objs.begin(); i != objs.end(); ++i) {
			if (i->type != TYPE_CLOSET) continue;
			if (i->get_sz_dim(!i->dim) >= 1.2*i->dz()) continue; // not a closet with a small door
			point center(i->get_cube_center());
			center[i->dim] = i->d[i->dim][i->dir]; // use center of door, not center of closet
			if (is_rotated()) {do_xy_rotate(bcube.get_cube_center(), center);}
			if (!check_door_dir(closest_to, in_dir, *i, center)) continue; // door is not in the correct direction, skip
			float const dist_sq(p2p_dist_sq(closest_to, center));
			if (closest_dist_sq == 0.0 || dist_sq < closest_dist_sq) {closest_dist_sq = dist_sq; door_ix = (i - objs.begin()); is_closet = 1;}
		} // for i
	}
	if (closest_dist_sq == 0.0) return 0; // no door found

	if (is_closet) {
		auto &obj(interior->room_geom->objs[door_ix]);
		if (obj.is_open()) {obj.flags &= ~RO_FLAG_OPEN;} // close
		else               {obj.flags |=  RO_FLAG_OPEN;} // open
		interior->room_geom->clear_static_vbos(); // need to regen object data
		play_door_open_close_sound(obj.get_cube_center(), obj.is_open());
	}
	else {toggle_door_state(door_ix, 1);} // toggle state if interior door; player_in_this_building=1
	return 1;
}

void building_t::toggle_door_state(unsigned door_ix, bool player_in_this_building) {
	assert(interior && door_ix < interior->doors.size());
	door_t &door(interior->doors[door_ix]);
	door.open ^= 1; // toggle open state
	clear_nav_graph(); // we just invalidated the AI navigation graph and must rebuild it; any in-progress paths may have people walking through closed doors
	interior->door_state_updated = 1; // required for AI navigation logic to adjust to this change
	interior->doors_to_update.push_back(door_ix);
	if (player_in_this_building) {play_door_open_close_sound(door.get_cube_center(), door.open);}
}

void building_t::play_door_open_close_sound(point const &pos, bool open) const {
	point pos_rot(pos);
	if (is_rotated()) {do_xy_rotate(bcube.get_cube_center(), pos_rot);}
	point const sound_pos(pos_rot + get_camera_coord_space_xlate()); // convert to camera space
	gen_sound((open ? (unsigned)SOUND_DOOR_OPEN : (unsigned)SOUND_DOOR_CLOSE), sound_pos);
}

// elevators

void building_t::update_elevators(point const &player_pos) {
	assert(interior);
	interior->update_elevators(player_pos, get_floor_thickness());
}
bool building_interior_t::update_elevators(point const &player_pos, float floor_thickness) { // Note: player_pos is in building space
	float const z_space(0.05*floor_thickness); // to prevent z-fighting
	static int prev_move_dir(2); // starts at not-moving

	// Note: the player can only be in one elevator at a time, so we can exit when we find the first containing elevator
	for (auto e = elevators.begin(); e != elevators.end(); ++e) { // find containing elevator (optimization + need to know z-range of elevator shaft)
		if (!e->contains_pt(player_pos)) continue; // player not in this elevator
		unsigned const elevator_id(e - elevators.begin());

		for (auto i = room_geom->objs.begin(); i != room_geom->objs.end(); ++i) { // find elevator car and see if player is in it
			if (i->type != TYPE_ELEVATOR || i->room_id != elevator_id || !i->contains_pt(player_pos)) continue;
			bool const move_dir(player_pos[!i->dim] < i->get_center_dim(!i->dim)); // player controls up/down direction based on which side of the elevator they stand on
			float dist(min(0.5f*CAMERA_RADIUS, 0.04f*i->dz()*fticks)*(move_dir ? 1.0 : -1.0)); // clamp to half camera radius to avoid falling through the floor for low framerates
			if (move_dir) {min_eq(dist, (e->z2() - i->z2() - z_space));} // going up
			else          {max_eq(dist, (e->z1() - i->z1() + z_space));} // going down
			if (fabs(dist) < 0.0001*z_space) break; // no movement, at top or bottom of elevator shaft (check with a tolerance)
			i->z1() += dist; i->z2() += dist;
			room_geom->mats_dynamic.clear(); // clear dynamic material vertex data (for all elevators) and recreate their VBOs
			if ((int)move_dir != prev_move_dir) {gen_sound(SOUND_SLIDING, get_camera_pos(), 0.2);} // play this sound quietly when the elevator starts moving or changes direction
			prev_move_dir = move_dir;
			return 1; // done
		} // for i
		break; // player in elevator shaft (on top of elevator?) but not inside elevator car, done
	} // for e
	prev_move_dir = 2;
	return 0;
}

// ray queries

// center, -x, +x, -y, +y, -z, +z
void get_sphere_boundary_pts(point const &center, float radius, point pts[7]) {
	pts[0] = center;

	for (unsigned dim = 0, ix = 1; dim < 3; ++dim) {
		vector3d dir(zero_vector);
		dir[dim]  = radius;
		pts[ix++] = center - dir;
		pts[ix++] = center + dir;
	}
}

bool building_t::is_pt_visible(point const &p1, point const &p2) const {
	if (!interior) return 1;
	if (is_light_occluded(p1, p2)) return 0; // okay to call for non-light point; checks walls, ceilings, and floors
	float const wall_thickness(get_wall_thickness());
	
	for (auto i = interior->doors.begin(); i != interior->doors.end(); ++i) {
		if (i->open) continue; // check only closed doors
		cube_t door(*i);
		door.expand_in_dim(i->dim, 0.5*wall_thickness); // increase door thickness
		if (door.line_intersects(p1, p2)) return 0;
	}
	return 1;
}
bool building_t::is_sphere_visible(point const &center, float radius, point const &pt) const {
	if (!interior) return 1;
	point pts[7];
	get_sphere_boundary_pts(center, radius, pts);
	for (unsigned n = 0; n < 7; ++n) {if (is_pt_visible(pts[n], pt)) return 1;}
	return 0;
}

bool building_t::is_pt_lit(point const &pt) const {
	if (!has_room_geom()) return 0; // no lights
	int const room_id(get_room_containing_pt(pt)); // call this only once on center in is_sphere_lit()?
	if (room_id < 0) return 0; // outside building?
	room_t const &room(get_room(room_id));

	for (auto i = interior->room_geom->objs.begin(); i != interior->room_geom->objs.end(); ++i) {
		if (!i->is_light_type() || !i->is_lit()) continue; // not a light, or light not on
		bool const same_room((int)i->room_id == room_id);
		//if (!same_room) continue; // different room (optimization); too strong?
		//bool const same_floor(fabs(i->z1() - pt.z) < floor_spacing); // doesn't work with lamps
		bool const same_floor(room.is_sec_bldg || get_floor_for_zval(pt.z) == get_floor_for_zval(i->z1()));
		if (!i->has_stairs() && !same_floor) continue; // different floors, and no stairs (optimization)
		if (same_floor && same_room) return 1; // same floor of same room, should be visible (optimization)
		point const center(i->get_cube_center());
		if (!dist_less_than(center, pt, 0.95*get_radius_for_room_light(*i))) continue;
		if (is_pt_visible(center, pt)) return 1; // likely returns true if same room
	} // for i
	return 0;
}
bool building_t::is_sphere_lit(point const &center, float radius) const {
	if (!has_room_geom()) return 0; // no lights (optimization)
	point pts[7];
	get_sphere_boundary_pts(center, radius, pts);
	for (unsigned n = 0; n < 7; ++n) {if (is_pt_lit(pts[n])) return 1;}
	return 0;
}

// object types

void setup_bldg_obj_types() {
	static bool was_setup(0);
	if (was_setup) return; // nothing to do
	was_setup = 1;
	// player_coll, ai_coll, pickup, attached, is_model, value, weight, name
	//                                                pc ac pu at im value  weight  name
	bldg_obj_types[TYPE_TABLE     ] = bldg_obj_type_t(1, 1, 0, 0, 0, 100.0, 100.0, "table");
	bldg_obj_types[TYPE_CHAIR     ] = bldg_obj_type_t(0, 1, 1, 0, 0, 50.0,  30.0,  "chair"); // skip player collisions because they can be in the way and block the path in some rooms
	bldg_obj_types[TYPE_STAIR     ] = bldg_obj_type_t(1, 0, 0, 1, 0, 0.0,   0.0,   "stair");
	bldg_obj_types[TYPE_STAIR_WALL] = bldg_obj_type_t(1, 1, 0, 1, 0, 0.0,   0.0,   "stairs wall");
	bldg_obj_types[TYPE_ELEVATOR  ] = bldg_obj_type_t(1, 1, 0, 1, 0, 0.0,   0.0,   "elevator");
	bldg_obj_types[TYPE_LIGHT     ] = bldg_obj_type_t(0, 0, 0, 0, 0, 40.0,  5.0,   "light");
	bldg_obj_types[TYPE_RUG       ] = bldg_obj_type_t(0, 0, 1, 0, 0, 50.0,  20.0,  "rug");
	bldg_obj_types[TYPE_PICTURE   ] = bldg_obj_type_t(0, 0, 1, 0, 0, 100.0, 10.0,  "picture"); // should be random value
	bldg_obj_types[TYPE_WBOARD    ] = bldg_obj_type_t(0, 0, 1, 0, 0, 50.0,  25.0,  "whiteboard");
	bldg_obj_types[TYPE_BOOK      ] = bldg_obj_type_t(0, 0, 1, 0, 0, 10.0,  1.0,   "book");
	bldg_obj_types[TYPE_BCASE     ] = bldg_obj_type_t(1, 1, 0, 1, 0, 150.0, 100.0, "bookcase");
	bldg_obj_types[TYPE_TCAN      ] = bldg_obj_type_t(0, 1, 1, 0, 0, 12.0,  2.0,   "trashcan"); // skip player collisions because they can be in the way and block the path in some rooms
	bldg_obj_types[TYPE_DESK      ] = bldg_obj_type_t(1, 1, 0, 0, 0, 100.0, 80.0,  "desk");
	bldg_obj_types[TYPE_BED       ] = bldg_obj_type_t(1, 1, 0, 0, 0, 300.0, 200.0, "bed");
	bldg_obj_types[TYPE_WINDOW    ] = bldg_obj_type_t(0, 0, 0, 1, 0, 0.0,   0.0,   "window");
	bldg_obj_types[TYPE_BLOCKER   ] = bldg_obj_type_t(0, 0, 0, 0, 0, 0.0,   0.0,   "<blocker>"); // not a drawn object; block other objects, but not the player or AI
	bldg_obj_types[TYPE_COLLIDER  ] = bldg_obj_type_t(1, 1, 0, 0, 0, 0.0,   0.0,   "<collider>"); // not a drawn object; block the player and AI
	bldg_obj_types[TYPE_CUBICLE   ] = bldg_obj_type_t(0, 0, 0, 1, 0, 500.0, 250.0, "cubicle"); // skip collisions because they have their own colliders
	bldg_obj_types[TYPE_STALL     ] = bldg_obj_type_t(1, 1, 0, 1, 0, 200.0, 150.0, "bathroom stall");
	bldg_obj_types[TYPE_SIGN      ] = bldg_obj_type_t(0, 0, 1, 0, 0, 10.0,  1.0,   "sign");
	bldg_obj_types[TYPE_COUNTER   ] = bldg_obj_type_t(1, 1, 0, 1, 0, 0.0,   0.0,   "kitchen counter");
	bldg_obj_types[TYPE_CABINET   ] = bldg_obj_type_t(0, 0, 0, 0, 0, 0.0,   0.0,   "kitchen cabinet");
	bldg_obj_types[TYPE_KSINK     ] = bldg_obj_type_t(1, 1, 0, 1, 0, 0.0,   0.0,   "kitchen sink");
	bldg_obj_types[TYPE_BRSINK    ] = bldg_obj_type_t(1, 1, 0, 1, 0, 0.0,   0.0,   "bathroom sink");
	bldg_obj_types[TYPE_PLANT     ] = bldg_obj_type_t(0, 0, 1, 0, 0, 20.0,  25.0,  "potted plant");
	bldg_obj_types[TYPE_DRESSER   ] = bldg_obj_type_t(1, 1, 0, 1, 0, 120.0, 120.0, "dresser");
	bldg_obj_types[TYPE_NIGHTSTAND] = bldg_obj_type_t(1, 1, 1, 0, 0, 60.0,  35.0,  "nightstand");
	bldg_obj_types[TYPE_FLOORING  ] = bldg_obj_type_t(0, 0, 0, 1, 0, 0.0,   0.0,   "flooring");
	bldg_obj_types[TYPE_CLOSET    ] = bldg_obj_type_t(1, 1, 0, 1, 0, 0.0,   0.0,   "closet");
	bldg_obj_types[TYPE_WALL_TRIM ] = bldg_obj_type_t(0, 0, 0, 1, 0, 0.0,   0.0,   "wall trim");
	bldg_obj_types[TYPE_RAILING   ] = bldg_obj_type_t(1, 0, 0, 1, 0, 0.0,   0.0,   "railing");
	bldg_obj_types[TYPE_CRATE     ] = bldg_obj_type_t(1, 1, 1, 0, 0, 20.0,  8.0,   "box"); // or crate, should be random value
	bldg_obj_types[TYPE_MIRROR    ] = bldg_obj_type_t(0, 0, 0, 0, 0, 0.0,   0.0,   "mirror");
	bldg_obj_types[TYPE_SHELVES   ] = bldg_obj_type_t(1, 1, 0, 0, 0, 0.0,   0.0,   "shelves");
	bldg_obj_types[TYPE_KEYBOARD  ] = bldg_obj_type_t(0, 0, 1, 0, 0, 15.0,  2.0,   "keyboard");
	bldg_obj_types[TYPE_SHOWER    ] = bldg_obj_type_t(1, 1, 0, 1, 0, 0.0,   0.0,   "shower");
	bldg_obj_types[TYPE_RDESK     ] = bldg_obj_type_t(1, 1, 0, 1, 0, 800.0, 400.0, "reception desk");
	bldg_obj_types[TYPE_BOTTLE    ] = bldg_obj_type_t(0, 0, 1, 0, 0, 1.0,   1.0,   "bottle");
	bldg_obj_types[TYPE_WINE_RACK ] = bldg_obj_type_t(1, 1, 1, 1, 0, 75.0,  40.0,  "wine rack");
	bldg_obj_types[TYPE_COMPUTER  ] = bldg_obj_type_t(0, 0, 1, 0, 0, 500.0, 20.0,  "computer");
	bldg_obj_types[TYPE_MWAVE     ] = bldg_obj_type_t(0, 0, 1, 0, 0, 100.0, 30.0,  "microwave oven");
	bldg_obj_types[TYPE_PAPER     ] = bldg_obj_type_t(0, 0, 1, 0, 0, 0.05,  0.01,  "sheet of paper"); // should be random value
	bldg_obj_types[TYPE_BLINDS    ] = bldg_obj_type_t(0, 0, 0, 0, 0, 0.0,   0.0,   "window blinds");
	bldg_obj_types[TYPE_PEN       ] = bldg_obj_type_t(0, 0, 1, 0, 0, 0.10,  0.02,  "pen");
	bldg_obj_types[TYPE_PENCIL    ] = bldg_obj_type_t(0, 0, 1, 0, 0, 0.10,  0.02,  "pencil");
	// 3D models
	bldg_obj_types[TYPE_TOILET    ] = bldg_obj_type_t(1, 1, 1, 1, 1, 120.0, 120.0, "toilet");
	bldg_obj_types[TYPE_SINK      ] = bldg_obj_type_t(1, 1, 1, 1, 1, 80.0,  80.0,  "sink");
	bldg_obj_types[TYPE_TUB       ] = bldg_obj_type_t(1, 1, 0, 1, 1, 250.0, 200.0, "bathtub");
	bldg_obj_types[TYPE_FRIDGE    ] = bldg_obj_type_t(1, 1, 1, 1, 1, 700.0, 300.0, "refrigerator");
	bldg_obj_types[TYPE_STOVE     ] = bldg_obj_type_t(1, 1, 1, 1, 1, 400.0, 200.0, "stove");
	bldg_obj_types[TYPE_TV        ] = bldg_obj_type_t(1, 1, 1, 0, 1, 400.0, 70.0,  "TV");
	bldg_obj_types[TYPE_MONITOR   ] = bldg_obj_type_t(1, 1, 1, 0, 1, 250.0, 15.0,  "computer monitor");
	bldg_obj_types[TYPE_COUCH     ] = bldg_obj_type_t(1, 1, 1, 0, 1, 600.0, 300.0, "couch");
	bldg_obj_types[TYPE_OFF_CHAIR ] = bldg_obj_type_t(1, 1, 1, 0, 1, 150.0, 60.0,  "office chair");
	bldg_obj_types[TYPE_URINAL    ] = bldg_obj_type_t(1, 1, 1, 1, 1, 100.0, 80.0,  "urinal");
	bldg_obj_types[TYPE_LAMP      ] = bldg_obj_type_t(0, 0, 1, 0, 1, 25.0,  12.0,  "lamp");
	bldg_obj_types[TYPE_WASHER    ] = bldg_obj_type_t(1, 1, 1, 1, 1, 300.0, 160.0, "washer");
	bldg_obj_types[TYPE_DRYER     ] = bldg_obj_type_t(1, 1, 1, 1, 1, 300.0, 180.0, "dryer");
	//                                                pc ac pu at im value  weight  name
}

// gameplay logic

void register_ai_player_coll(pedestrian_t const &person) {
	static double last_coll_time(0.0);
	
	if (tfticks - last_coll_time > 2.0*TICKS_PER_SECOND) {
		gen_sound(SOUND_SCREAM1, get_camera_pos());
		last_coll_time = tfticks;
	}
	add_camera_filter(colorRGBA(RED, 0.25), 1, -1, CAM_FILT_DAMAGE); // 4 ticks of red damage
}

