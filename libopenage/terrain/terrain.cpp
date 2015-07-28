// Copyright 2013-2015 the openage authors. See copying.md for legal info.

#include "terrain.h"

#include <memory>
#include <set>
#include <unordered_map>

#include "../log/log.h"
#include "../error/error.h"
#include "../engine.h"
#include "../texture.h"
#include "../coord/camgame.h"
#include "../coord/chunk.h"
#include "../coord/tile.h"
#include "../coord/tile3.h"
#include "../util/dir.h"
#include "../util/misc.h"
#include "../util/strings.h"

#include "terrain_chunk.h"

namespace openage {

TileContent::TileContent() :
	terrain_id{0} {
}

TileContent::~TileContent() {}

Terrain::Terrain(AssetManager &assetmanager,
                 const std::vector<gamedata::terrain_type> &terrain_meta,
                 const std::vector<gamedata::blending_mode> &blending_meta,
                 bool is_infinite)
	:
	infinite{is_infinite} {

	// TODO:
	//this->limit_positive =
	//this->limit_negative =

	// maps chunk position to chunks
	this->chunks = std::unordered_map<coord::chunk, TerrainChunk *, coord_chunk_hash>{};

	// activate blending
	this->blending_enabled = true;

	this->terrain_id_count         = terrain_meta.size();
	this->blendmode_count          = blending_meta.size();
	this->textures.reserve(this->terrain_id_count);
	this->blending_masks.reserve(this->blendmode_count);
	this->terrain_id_priority_map  = std::make_unique<int[]>(this->terrain_id_count);
	this->terrain_id_blendmode_map = std::make_unique<int[]>(this->terrain_id_count);
	this->influences_buf           = std::make_unique<struct influence[]>(this->terrain_id_count);


	log::log(MSG(dbg) << "Terrain prefs: " <<
		"tiletypes=" << this->terrain_id_count << ", "
		"blendmodes=" << this->blendmode_count);

	// create tile textures (snow, ice, grass, whatever)
	for (size_t i = 0; i < this->terrain_id_count; i++) {
		auto line = &terrain_meta[i];
		terrain_t terrain_id = line->terrain_id;
		this->validate_terrain(terrain_id);

		// TODO: terrain double-define check?
		this->terrain_id_priority_map[terrain_id]  = line->blend_priority;
		this->terrain_id_blendmode_map[terrain_id] = line->blend_mode;

		// TODO: remove hardcoding and rely on nyan data
		auto terraintex_filename = util::sformat("converted/terrain/%d.slp.png", line->slp_id);
		auto new_texture = assetmanager.get_texture(terraintex_filename);

		this->textures[terrain_id] = new_texture;
	}

	// create blending masks (see doc/media/blendomatic)
	for (size_t i = 0; i < this->blendmode_count; i++) {
		auto line = &blending_meta[i];

		std::string mask_filename = util::sformat("converted/blendomatic/mode%02d.png", line->blend_mode);
		this->blending_masks[i] = assetmanager.get_texture(mask_filename);
	}

}

Terrain::~Terrain() {
	log::log(MSG(dbg) << "Cleanup terrain");

	for (auto &chunk : this->chunks) {
		// this chunk was autogenerated, so clean it up
		if (chunk.second->manually_created == false) {
			delete chunk.second;
		}
	}
}

std::vector<coord::chunk> Terrain::used_chunks() const {
	std::vector<coord::chunk> result;
	for (auto &c : chunks) {
		result.push_back(c.first);
	}
	return result;
}

bool Terrain::fill(const int *data, coord::tile_delta size) {
	bool was_cut = false;

	coord::tile pos = {0, 0};
	for (; pos.ne < size.ne; pos.ne++) {
		for (pos.se = 0; pos.se < size.se; pos.se++) {
			if (this->check_tile(pos) == tile_state::invalid) {
				was_cut = true;
				continue;
			}
			int terrain_id = data[pos.ne * size.ne + pos.se];
			TerrainChunk *chunk = this->get_create_chunk(pos);
			chunk->get_data(pos)->terrain_id = terrain_id;
		}
	}
	return was_cut;
}

void Terrain::attach_chunk(TerrainChunk *new_chunk,
                           coord::chunk position,
                           bool manually_created) {
	new_chunk->set_terrain(this);
	new_chunk->manually_created = manually_created;
	log::log(MSG(dbg) << "Inserting new chunk at (" << position.ne << "," << position.se << ")");
	this->chunks[position] = new_chunk;

	struct chunk_neighbors neigh = this->get_chunk_neighbors(position);
	for (int i = 0; i < 8; i++) {
		TerrainChunk *neighbor = neigh.neighbor[i];
		if (neighbor != nullptr) {
			//set the new chunks neighbor to the neighbor chunk
			new_chunk->neighbors.neighbor[i] = neighbor;

			//set the neighbors neighbor on the opposite direction
			//to the new chunk
			neighbor->neighbors.neighbor[(i+4) % 8] = new_chunk;

			log::log(MSG(dbg) << "Neighbor " << i << " gets notified of new neighbor.");
		}
		else {
			log::log(MSG(dbg) << "Neighbor " << i << " not found.");
		}
	}
}

TerrainChunk *Terrain::get_chunk(coord::chunk position) {
	auto iter = this->chunks.find(position);

	if (iter == this->chunks.end()) {
		return nullptr;
	}
	else {
		return iter->second;
	}
}

TerrainChunk *Terrain::get_chunk(coord::tile position) {
	return this->get_chunk(position.to_chunk());
}

TerrainChunk *Terrain::get_create_chunk(coord::chunk position) {
	TerrainChunk *res = this->get_chunk(position);
	if (res == nullptr) {
		res = new TerrainChunk();
		this->attach_chunk(res, position, false);
	}
	return res;
}

TerrainChunk *Terrain::get_create_chunk(coord::tile position) {
	return this->get_create_chunk(position.to_chunk());
}

TileContent *Terrain::get_data(coord::tile position) {
	TerrainChunk *c = this->get_chunk(position.to_chunk());
	if (c == nullptr) {
		return nullptr;
	} else {
		return c->get_data(position.get_pos_on_chunk().to_tile());
	}
}

TerrainObject *Terrain::obj_at_point(const coord::phys3 &point) {
	coord::tile t = point.to_tile3().to_tile();
	TileContent *tc = this->get_data(t);
	if (!tc) {
		return nullptr;
	}
	for (auto obj_ptr : tc->obj) {
		if (obj_ptr->contains(point)) {
			return obj_ptr;
		}
	}
	return nullptr;
}

bool Terrain::validate_terrain(terrain_t terrain_id) {
	if (terrain_id >= (ssize_t)this->terrain_id_count) {
		throw Error(MSG(err) << "Requested terrain_id is out of range: " << terrain_id);
	}
	else {
		return true;
	}
}

bool Terrain::validate_mask(ssize_t mask_id) {
	if (mask_id >= (ssize_t)this->blendmode_count) {
		throw Error(MSG(err) << "Requested mask_id is out of range: " << mask_id);
	}
	else {
		return true;
	}
}

int Terrain::priority(terrain_t terrain_id) {
	this->validate_terrain(terrain_id);
	return this->terrain_id_priority_map[terrain_id];
}

int Terrain::blendmode(terrain_t terrain_id) {
	this->validate_terrain(terrain_id);
	return this->terrain_id_blendmode_map[terrain_id];
}

Texture *Terrain::texture(terrain_t terrain_id) {
	this->validate_terrain(terrain_id);
	return this->textures[terrain_id];
}

Texture *Terrain::blending_mask(ssize_t mask_id) {
	this->validate_mask(mask_id);
	return this->blending_masks[mask_id];
}

unsigned Terrain::get_subtexture_id(coord::tile pos, unsigned atlas_size) {
	unsigned result = 0;

	result += util::mod<coord::tile_t>(pos.se, atlas_size);
	result *= atlas_size;
	result += util::mod<coord::tile_t>(pos.ne, atlas_size);

	return result;
}

struct chunk_neighbors Terrain::get_chunk_neighbors(coord::chunk position) {
	struct chunk_neighbors ret;
	coord::chunk tmp_pos;

	for (int i = 0; i < 8; i++) {
		tmp_pos = position;
		// TODO: use the overloaded operators..
		tmp_pos.ne += neigh_offsets[i].ne;
		tmp_pos.se += neigh_offsets[i].se;
		ret.neighbor[i] = this->get_chunk(tmp_pos);
	}

	return ret;
}

int Terrain::get_blending_mode(terrain_t base_id, terrain_t neighbor_id) {
	/*
	 * this function may require much more code, but this simple
	 * magnitude comparison seems to do the job.
	 * feel free to confirm or fix the behavior.
	 *
	 * my guess is that the blending mode encodes another information
	 * not publicly noticed yet: the overlay priority.
	 * the higher the blendmode id, the higher the mode priority.
	 * this may also be the reason why there are mask duplicates
	 * in blendomatic.dat
	 *
	 * funny enough, just using the modes in the dat file lead
	 * to a totally wrong render. the convert script reassigns the
	 * blending modes with a simple key=>val mapping,
	 * and after that, it looks perfect.
	 */

	int base_mode     = this->blendmode(base_id);
	int neighbor_mode = this->blendmode(neighbor_id);

	if (neighbor_mode > base_mode) {
		return neighbor_mode;
	} else {
		return base_mode;
	}
}

tile_state Terrain::check_tile(coord::tile position) {
	if (this->check_tile_position(position) == false) {
		return tile_state::invalid;
	}
	else {
		TerrainChunk *chunk = this->get_chunk(position);
		if (chunk == nullptr) {
			return tile_state::creatable;
		}
		else {
			return tile_state::existing;
		}
	}
}

bool Terrain::check_tile_position(coord::tile pos) {
	if (this->infinite == true) {
		return true;
	}

	if (pos.ne < this->limit_negative.ne
	    || pos.se < this->limit_negative.se
	    || pos.ne > this->limit_positive.ne
	    || pos.se > this->limit_positive.se) {
		return false;
	}
	else {
		return true;
	}

}

void Terrain::draw(Engine *engine) {
	// TODO: move this draw invokation to a render manager.
	//       it can reorder the draw instructions and minimize texture switching.

	// top left, bottom right tile coordinates
	// that are currently visible in the window
	coord::tile tl, tr, bl, br;
	coord::window wtl, wtr, wbl, wbr;

	// query the window coordinates from the engine first
	wtl = coord::window{                    0,                     0};
	wtr = coord::window{engine->engine_coord_data->window_size.x,                     0};
	wbl = coord::window{                    0, engine->engine_coord_data->window_size.y};
	wbr = coord::window{engine->engine_coord_data->window_size.x, engine->engine_coord_data->window_size.y};

	// then convert them to tile coordinates.
	tl = wtl.to_camgame().to_phys3(0).to_phys2().to_tile();
	tr = wtr.to_camgame().to_phys3(0).to_phys2().to_tile();
	bl = wbl.to_camgame().to_phys3(0).to_phys2().to_tile();
	br = wbr.to_camgame().to_phys3(0).to_phys2().to_tile();

	// main terrain calculation call: get the `terrain_render_data`
	auto draw_data = this->create_draw_advice(tl, tr, br, bl);

	// TODO: the following loop is totally inefficient and shit.
	//       it reloads the drawing texture to the gpu FOR EACH TILE!
	//       nevertheless, currently it works.

	// draw the terrain ground
	for (auto &tile : draw_data.tiles) {

		// iterate over all layers to be drawn
		for (int i = 0; i < tile.count; i++) {
			struct tile_data *layer = &tile.data[i];

			// position, where the tile is drawn
			coord::tile tile_pos = layer->pos;

			int      mask_id       = layer->mask_id;
			Texture *texture       = layer->tex;
			int      subtexture_id = layer->subtexture_id;
			Texture *mask_texture  = layer->mask_tex;

			texture->draw(tile_pos, ALPHAMASKED, subtexture_id, mask_texture, mask_id);
		}
	}

	// TODO: drawing buildings can't be the job of the terrain..
	// draw the buildings
	for (auto &object : draw_data.objects) {
		object->draw();
	}
}

struct terrain_render_data Terrain::create_draw_advice(coord::tile ab,
                                                       coord::tile cd,
                                                       coord::tile ef,
                                                       coord::tile gh) {

	/*
	 * The passed parameters define the screen corners.
	 *
	 *    ne, se coordinates
	 *    o = screen corner, where the tile coordinates can be queried.
	 *    x = corner of the rhombus that will be drawn, calculated by all o.
	 *
	 *                  cb
	 *                   x
	 *                 .   .
	 *               .       .
	 *          ab o===========o cd
	 *           . =  visible  = .
	 *      gb x   =  screen   =   x cf
	 *           . =           = .
	 *          gh o===========o ef
	 *               .       .
	 *                 .   .
	 *                   x
	 *                  gf
	 *
	 * The rendering area may be optimized further in the future,
	 * to exactly fit the visible screen.
	 * For now, we are drawing the big rhombus.
	 */

	// procedure: find all the tiles to be drawn
	// and store them to a tile drawing instruction structure
	struct terrain_render_data data;

	// vector of tiles to be drawn
	std::vector<struct tile_draw_data> *tiles = &data.tiles;

	// ordered set of objects on the terrain (buildings.)
	// it's ordered by the visibility layers.
	auto objects = &data.objects;

	coord::tile gb = {gh.ne, ab.se};
	coord::tile cf = {cd.ne, ef.se};

	// hint the vector about the number of tiles it will contain
	size_t tiles_count = std::abs(cf.ne - gb.ne) * std::abs(cf.se - gb.se);
	tiles->reserve(tiles_count);

	// sweep the whole rhombus area
	for (coord::tile tilepos = gb; tilepos.ne <= (ssize_t) cf.ne; tilepos.ne++) {
		for (tilepos.se = gb.se; tilepos.se <= (ssize_t) cf.se; tilepos.se++) {

			// get the terrain tile drawing data
			auto tile = this->create_tile_advice(tilepos);
			tiles->push_back(tile);

			// get the object standing on the tile
			// TODO: make the terrain independent of objects standing on it.
			TileContent *tile_content = this->get_data(tilepos);
			if (tile_content != nullptr) {
				for (auto obj_item : tile_content->obj) {
					objects->insert(obj_item);
				}
			}
		}
	}

	return data;
}


struct tile_draw_data Terrain::create_tile_advice(coord::tile position) {
	// this struct will be filled with all tiles and overlays to draw.
	struct tile_draw_data tile;
	tile.count = 0;

	TileContent *base_tile_content = this->get_data(position);

	// chunk of this tile does not exist
	if (base_tile_content == nullptr) {
		return tile;
	}

	struct tile_data base_tile_data;

	// the base terrain id of the tile
	base_tile_data.terrain_id = base_tile_content->terrain_id;

	// the base terrain is not existant.
	if (base_tile_data.terrain_id < 0) {
		return tile;
	}

	this->validate_terrain(base_tile_data.terrain_id);

	base_tile_data.state         = tile_state::existing;
	base_tile_data.pos           = position;
	base_tile_data.priority      = this->priority(base_tile_data.terrain_id);
	base_tile_data.tex           = this->texture(base_tile_data.terrain_id);
	base_tile_data.subtexture_id = this->get_subtexture_id(position, base_tile_data.tex->atlas_dimensions);
	base_tile_data.blend_mode    = -1;
	base_tile_data.mask_tex      = nullptr;
	base_tile_data.mask_id       = -1;

	tile.data[tile.count] = base_tile_data;
	tile.count += 1;

	// blendomatic!!111
	//  see doc/media/blendomatic for the idea behind this.
	if (this->blending_enabled) {

		// the neighbors of the base tile
		struct neighbor_tile neigh_data[8];

		// get all neighbor tiles around position, reset the influence directions.
		this->get_neighbors(position, neigh_data, this->influences_buf.get());

		// create influence list (direction, priority)
		// strip and order influences, get the final influence data structure
		struct influence_group influence_group = this->calculate_influences(
			&base_tile_data, neigh_data,
			this->influences_buf.get());

		// create the draw_masks from the calculated influences
		this->calculate_masks(position, &tile, &influence_group);
	}

	return tile;
}

void Terrain::get_neighbors(coord::tile basepos,
                            neighbor_tile *neigh_data,
                            influence *influences_by_terrain_id) {
	// walk over all given neighbor tiles and store them to the influence list,
	// group them by terrain id.

	for (int neigh_id = 0; neigh_id < 8; neigh_id++) {

		// the current neighbor
		auto neighbor = &neigh_data[neigh_id];

		// calculate the pos of the neighbor tile
		coord::tile neigh_pos = basepos + neigh_offsets[neigh_id];

		// get the neighbor data
		TileContent *neigh_content = this->get_data(neigh_pos);

		// chunk for neighbor or single tile is not existant
		if (neigh_content == nullptr || neigh_content->terrain_id < 0) {
			neighbor->state = tile_state::missing;
		}
		else {
			neighbor->terrain_id = neigh_content->terrain_id;
			neighbor->state      = tile_state::existing;
			neighbor->priority   = this->priority(neighbor->terrain_id);

			// reset influence directions for this tile
			influences_by_terrain_id[neighbor->terrain_id].direction = 0;
		}
	}
}

struct influence_group Terrain::calculate_influences(struct tile_data *base_tile,
                                                     struct neighbor_tile *neigh_data,
                                                     struct influence *influences_by_terrain_id) {
	// influences to actually draw (-> maximum 8)
	struct influence_group influences;
	influences.count = 0;

	// process adjacent neighbors first,
	// then add diagonal influences, if no adjacent influence was found
	constexpr int neigh_id_lookup[] = {1, 3, 5, 7, 0, 2, 4, 6};

	for (int i = 0; i < 8; i++) {
		// diagonal neighbors: (neigh_id % 2) == 0
		// adjacent neighbors: (neigh_id % 2) == 1

		int neigh_id = neigh_id_lookup[i];
		bool is_adjacent_neighbor = neigh_id % 2 == 1;
		bool is_diagonal_neighbor = not is_adjacent_neighbor;

		// the current neighbor_tile.
		auto neighbor = &neigh_data[neigh_id];

		// neighbor is nonexistant
		if (neighbor->state == tile_state::missing) {
			continue;
		}

		// neighbor only interesting if it's a different terrain than the base.
		// if it is the same id, the priorities are equal.
		// neighbor draws over the base if it's priority is greater.
		if (neighbor->priority > base_tile->priority) {

			// get influence storage for the neighbor terrain id
			// to group influences by id
			auto influence = &influences_by_terrain_id[neighbor->terrain_id];

			// check if diagonal influence is valid
			if (is_diagonal_neighbor) {
				// get the adjacent neighbors to the current diagonal
				// influence
				//  (a & 0x07) == (a % 8)
				uint8_t adj_neigh_0 = (neigh_id - 1) & 0x07;
				uint8_t adj_neigh_1 = (neigh_id + 1) & 0x07;

				uint8_t neigh_mask = (1 << adj_neigh_0) | (1 << adj_neigh_1);

				// the adjacent neigbors are already influencing
				// the current tile, therefore don't apply the diagonal mask
				if ((influence->direction & neigh_mask) != 0) {
					continue;
				}
			}

			// this terrain id hasn't had influence so far:
			// add it to the list of influences.
			if (influence->direction == 0) {
				influences.terrain_ids[influences.count] = neighbor->terrain_id;
				influences.count += 1;
			}

			// as tile i has influence for this priority
			//  => bit i is set to 1 by 2^i
			influence->direction |= 1 << neigh_id;
			influence->priority = neighbor->priority;
			influence->terrain_id = neighbor->terrain_id;
		}
	}

	// influences_by_terrain_id will be merged in the following,
	// unused terrain ids will be dropped now.

	// shrink the big influence buffer that had entries for all terrains
	// by copying the possible (max 8) influences to a separate buffer.
	for (int k = 0; k < influences.count; k++) {
		int relevant_id    = influences.terrain_ids[k];
		influences.data[k] = influences_by_terrain_id[relevant_id];
	}

	// order the influences by their priority
	for (int k = 1; k < influences.count; k++) {
		struct influence tmp_influence = influences.data[k];

		int l = k - 1;
		while (l >= 0 && influences.data[l].priority > tmp_influence.priority) {
			influences.data[l + 1] = influences.data[l];
			l -= 1;
		}

		influences.data[l + 1] = tmp_influence;
	}

	return influences;
}


void Terrain::calculate_masks(coord::tile position,
                              struct tile_draw_data *tile_data,
                              struct influence_group *influences) {

	// influences are grouped by terrain id.
	// the direction member has each bit set to 1 that is an influence from that direction.
	// create a mask for this direction combination.

	// the base tile is stored at position 0 of the draw_mask
	terrain_t base_terrain_id = tile_data->data[0].terrain_id;

	// iterate over all neighbors (with different terrain_ids) that have influence
	for (ssize_t i = 0; i < influences->count; i++) {

		// neighbor id of the current influence
		char direction_bits = influences->data[i].direction;

		// all bits are 0 -> no influence directions stored.
		//  => no influence can be ignored.
		if (direction_bits == 0) {
			continue;
		}

		terrain_t neighbor_terrain_id = influences->data[i].terrain_id;
		int adjacent_mask_id = -1;

		/* neighbor ids:
		     0
		   7   1      => 8 neighbors that can have influence on
		 6   @   2         the mask id selection.
		   5   3
		     4
		*/

		// filter adjacent and diagonal influences    neighbor_id: 76543210
		uint8_t direction_bits_adjacent = direction_bits & 0xAA; //0b10101010
		uint8_t direction_bits_diagonal = direction_bits & 0x55; //0b01010101

		switch (direction_bits_adjacent) {
		case 0x08:  //0b00001000
			adjacent_mask_id = 0;  //0..3
			break;
		case 0x02:  //0b00000010
			adjacent_mask_id = 4;  //4..7
			break;
		case 0x20:  //0b00100000
			adjacent_mask_id = 8;  //8..11
			break;
		case 0x80:  //0b10000000
			adjacent_mask_id = 12; //12..15
			break;
		case 0x22:  //0b00100010
			adjacent_mask_id = 20;
			break;
		case 0x88:  //0b10001000
			adjacent_mask_id = 21;
			break;
		case 0xA0:  //0b10100000
			adjacent_mask_id = 22;
			break;
		case 0x82:  //0b10000010
			adjacent_mask_id = 23;
			break;
		case 0x28:  //0b00101000
			adjacent_mask_id = 24;
			break;
		case 0x0A:  //0b00001010
			adjacent_mask_id = 25;
			break;
		case 0x2A:  //0b00101010
			adjacent_mask_id = 26;
			break;
		case 0xA8:  //0b10101000
			adjacent_mask_id = 27;
			break;
		case 0xA2:  //0b10100010
			adjacent_mask_id = 28;
			break;
		case 0x8A:  //0b10001010
			adjacent_mask_id = 29;
			break;
		case 0xAA:  //0b10101010
			adjacent_mask_id = 30;
			break;
		}

		// if it's the linear adjacent mask, cycle the 4 possible masks.
		// e.g. long shorelines don't look the same then.
		//  maskid == 0x08 0x02 0x80 0x20 for that.
		if (adjacent_mask_id <= 12 && adjacent_mask_id % 4 == 0) {
			//we have 4 = 2^2 anti redundancy masks, so keep the last 2 bits
			uint8_t anti_redundancy_offset = (position.ne + position.se) & 0x03;
			adjacent_mask_id += anti_redundancy_offset;
		}

		// get the blending mode (the mask selection) for this transition
		// the mode is dependent on the two meeting terrain types
		int blend_mode = this->get_blending_mode(base_terrain_id, neighbor_terrain_id);

		// append the mask for the adjacent blending
		if (adjacent_mask_id >= 0) {
			struct tile_data *overlay = &tile_data->data[tile_data->count];
			overlay->pos        = position;
			overlay->mask_id    = adjacent_mask_id;
			overlay->blend_mode = blend_mode;
			overlay->terrain_id = neighbor_terrain_id;
			overlay->tex        = this->texture(neighbor_terrain_id);
			overlay->subtexture_id = this->get_subtexture_id(position, overlay->tex->atlas_dimensions);
			overlay->mask_tex   = this->blending_mask(blend_mode);
			overlay->state      = tile_state::existing;

			tile_data->count += 1;
		}

		// append the mask for the diagonal blending
		if (direction_bits_diagonal > 0) {
			for (int l = 0; l < 4; l++) {
				// generate one mask for each influencing diagonal neighbor id.
				// even if they all have the same terrain_id,
				// because we don't have combined diagonal influence masks.

				// l == 0: pos = 0b000000001, mask = 18
				// l == 1: pos = 0b000000100, mask = 16
				// l == 2: pos = 0b000010000, mask = 17
				// l == 3: pos = 0b001000000, mask = 19

				int current_direction_bit = 1 << (l*2);
				constexpr int diag_mask_id_map[4] = {18, 16, 17, 19};

				if (direction_bits_diagonal & current_direction_bit) {
					struct tile_data *overlay = &tile_data->data[tile_data->count];
					overlay->pos        = position;
					overlay->mask_id    = diag_mask_id_map[l];
					overlay->blend_mode = blend_mode;
					overlay->terrain_id = neighbor_terrain_id;
					overlay->tex        = this->texture(neighbor_terrain_id);
					overlay->subtexture_id = this->get_subtexture_id(position, overlay->tex->atlas_dimensions);
					overlay->mask_tex   = this->blending_mask(blend_mode);
					overlay->state      = tile_state::existing;

					tile_data->count += 1;
				}
			}
		}
	}
}

} // namespace openage