#include "raycast.h"
#include "../meshers/blocky/voxel_mesher_blocky.h"
#include "../meshers/cubes/voxel_mesher_cubes.h"
#include "../storage/voxel_buffer.h"
#include "../storage/voxel_data.h"
#include "../terrain/voxel_node.h"
#include "../util/godot/classes/ref_counted.h"
#include "../util/voxel_raycast.h"
#include "funcs.h"
#include "voxel_raycast_result.h"

namespace zylann::voxel {

// Binary search can be more accurate than linear regression because the SDF can be inaccurate in the first place.
// An alternative would be to polygonize a tiny area around the middle-phase hit position.
// `d1` is how far from `pos0` along `dir` the binary search will take place.
// The segment may be adjusted internally if it does not contain a zero-crossing of the
template <typename Volume_F>
float approximate_distance_to_isosurface_binary_search(
		const Volume_F &f,
		const Vector3 pos0,
		const Vector3 dir,
		float d1,
		const int iterations
) {
	float d0 = 0.f;
	float sdf0 = get_sdf_interpolated(f, pos0);
	// The position given as argument may be a rough approximation coming from the middle-phase,
	// so it can be slightly below the surface. We can adjust it a little so it is above.
	for (int i = 0; i < 4 && sdf0 < 0.f; ++i) {
		d0 -= 0.5f;
		sdf0 = get_sdf_interpolated(f, pos0 + dir * d0);
	}

	float sdf1 = get_sdf_interpolated(f, pos0 + dir * d1);
	for (int i = 0; i < 4 && sdf1 > 0.f; ++i) {
		d1 += 0.5f;
		sdf1 = get_sdf_interpolated(f, pos0 + dir * d1);
	}

	if ((sdf0 > 0) != (sdf1 > 0)) {
		// Binary search
		for (int i = 0; i < iterations; ++i) {
			const float dm = 0.5f * (d0 + d1);
			const float sdf_mid = get_sdf_interpolated(f, pos0 + dir * dm);

			if ((sdf_mid > 0) != (sdf0 > 0)) {
				sdf1 = sdf_mid;
				d1 = dm;
			} else {
				sdf0 = sdf_mid;
				d0 = dm;
			}
		}
	}

	// Pick distance closest to the surface
	if (Math::abs(sdf0) < Math::abs(sdf1)) {
		return d0;
	} else {
		return d1;
	}
}

Ref<VoxelRaycastResult> raycast_sdf(
		const VoxelData &voxel_data,
		const Vector3 ray_origin,
		const Vector3 ray_dir,
		const float max_distance,
		const uint8_t binary_search_iterations
) {
	// TODO Implement reverse raycast? (going from inside ground to air, could be useful for undigging)

	// TODO Optimization: voxel raycast uses `get_voxel` which is the slowest, but could be made faster.
	// Instead, do a broad-phase on blocks. If a block's voxels need to be parsed, get all positions the ray could go
	// through in that block, then query them all at once (better for bulk processing without going again through
	// locking and data structures, and allows SIMD). Then check results in order.
	// If no hit is found, carry on with next blocks.

	struct RaycastPredicate {
		const VoxelData &data;

		bool operator()(const VoxelRaycastState &rs) {
			// This is not particularly optimized, but runs fast enough for player raycasts
			VoxelSingleValue defval;
			defval.f = constants::SDF_FAR_OUTSIDE;
			const VoxelSingleValue v = data.get_voxel(rs.hit_position, VoxelBuffer::CHANNEL_SDF, defval);
			return v.f < 0;
		}
	};

	Ref<VoxelRaycastResult> res;

	// We use grid-raycast as a middle-phase to roughly detect where the hit will be
	RaycastPredicate predicate = { voxel_data };
	Vector3i hit_pos;
	Vector3i prev_pos;
	float hit_distance;
	float hit_distance_prev;
	// Voxels polygonized using marching cubes influence a region centered on their lower corner,
	// and extend up to 0.5 units in all directions.
	//
	//   o--------o--------o
	//   | A      |     B  |  Here voxel B is full, voxels A, C and D are empty.
	//   |       xxx       |  Matter will show up at the lower corner of B due to interpolation.
	//   |     xxxxxxx     |
	//   o---xxxxxoxxxxx---o
	//   |     xxxxxxx     |
	//   |       xxx       |
	//   | C      |     D  |
	//   o--------o--------o
	//
	// `voxel_raycast` operates on a discrete grid of cubic voxels, so to account for the smooth interpolation,
	// we may offset the ray so that cubes act as if they were centered on the filtered result.
	const Vector3 offset(0.5, 0.5, 0.5);
	if (voxel_raycast(
				ray_origin + offset,
				ray_dir,
				predicate,
				max_distance,
				hit_pos,
				prev_pos,
				hit_distance,
				hit_distance_prev
		)) {
		// Approximate surface

		float d = hit_distance;

		if (binary_search_iterations > 0) {
			// This is not particularly optimized, but runs fast enough for player raycasts
			struct VolumeSampler {
				const VoxelData &data;

				inline float operator()(const Vector3i &pos) const {
					VoxelSingleValue defval;
					defval.f = constants::SDF_FAR_OUTSIDE;
					const VoxelSingleValue value = data.get_voxel(pos, VoxelBuffer::CHANNEL_SDF, defval);
					return value.f;
				}
			};

			VolumeSampler sampler{ voxel_data };
			d = hit_distance_prev +
					approximate_distance_to_isosurface_binary_search(
							sampler,
							ray_origin + ray_dir * hit_distance_prev,
							ray_dir,
							hit_distance - hit_distance_prev,
							binary_search_iterations
					);
		}

		res.instantiate();
		res->position = hit_pos;
		res->previous_position = prev_pos;
		res->distance_along_ray = d;
	}

	return res;
}

Ref<VoxelRaycastResult> raycast_blocky(
		const VoxelData &voxel_data,
		const VoxelMesherBlocky &mesher,
		const Vector3 ray_origin,
		const Vector3 ray_dir,
		const float max_distance,
		const uint32_t p_collision_mask
) {
	struct RaycastPredicateBlocky {
		const VoxelData &data;
		const VoxelBlockyLibraryBase::BakedData &baked_data;
		const uint32_t collision_mask;
		const Vector3 p_from;
		const Vector3 p_to;

		bool operator()(const VoxelRaycastState &rs) const {
			VoxelSingleValue defval;
			defval.i = 0;
			const int v = data.get_voxel(rs.hit_position, VoxelBuffer::CHANNEL_TYPE, defval).i;

			if (baked_data.has_model(v) == false) {
				return false;
			}

			const VoxelBlockyModel::BakedData &model = baked_data.models[v];
			if ((model.box_collision_mask & collision_mask) == 0) {
				return false;
			}

			for (const AABB &aabb : model.box_collision_aabbs) {
				if (AABB(aabb.position + rs.hit_position, aabb.size).intersects_segment(p_from, p_to)) {
					return true;
				}
			}

			return false;
		}
	};

	Ref<VoxelRaycastResult> res;

	Ref<VoxelBlockyLibraryBase> library_ref = mesher.get_library();
	if (library_ref.is_null()) {
		return res;
	}

	RaycastPredicateBlocky predicate{
		voxel_data, //
		library_ref->get_baked_data(), //
		p_collision_mask, //
		ray_origin, //
		ray_origin + ray_dir * max_distance //
	};

	float hit_distance;
	float hit_distance_prev;
	Vector3i hit_pos;
	Vector3i prev_pos;

	if (zylann::voxel_raycast(
				ray_origin, ray_dir, predicate, max_distance, hit_pos, prev_pos, hit_distance, hit_distance_prev
		)) {
		res.instantiate();
		res->position = hit_pos;
		res->previous_position = prev_pos;
		res->distance_along_ray = hit_distance;
	}

	return res;
}

Ref<VoxelRaycastResult> raycast_nonzero(
		const VoxelData &voxel_data,
		const Vector3 ray_origin,
		const Vector3 ray_dir,
		const float max_distance,
		const uint8_t p_channel
) {
	struct RaycastPredicateColor {
		const VoxelData &data;
		const uint8_t channel;

		bool operator()(const VoxelRaycastState &rs) const {
			VoxelSingleValue defval;
			defval.i = 0;
			const uint64_t v = data.get_voxel(rs.hit_position, channel, defval).i;
			return v != 0;
		}
	};

	Ref<VoxelRaycastResult> res;

	RaycastPredicateColor predicate{ voxel_data, p_channel };

	float hit_distance;
	float hit_distance_prev;
	Vector3i hit_pos;
	Vector3i prev_pos;

	if (zylann::voxel_raycast(
				ray_origin, ray_dir, predicate, max_distance, hit_pos, prev_pos, hit_distance, hit_distance_prev
		)) {
		res.instantiate();
		res->position = hit_pos;
		res->previous_position = prev_pos;
		res->distance_along_ray = hit_distance;
	}

	return res;
}

Ref<VoxelRaycastResult> raycast_generic(
		const VoxelData &voxel_data,
		const Ref<VoxelMesher> mesher,
		const Vector3 ray_origin,
		const Vector3 ray_dir,
		const float max_distance,
		const uint32_t p_collision_mask,
		const uint8_t binary_search_iterations
) {
	using namespace zylann::godot;

	Ref<VoxelRaycastResult> res;

	Ref<VoxelMesherBlocky> mesher_blocky;
	Ref<VoxelMesherCubes> mesher_cubes;

	if (try_get_as(mesher, mesher_blocky)) {
		res = raycast_blocky(voxel_data, **mesher_blocky, ray_origin, ray_dir, max_distance, p_collision_mask);

	} else if (try_get_as(mesher, mesher_cubes)) {
		res = raycast_nonzero(voxel_data, ray_origin, ray_dir, max_distance, VoxelBuffer::CHANNEL_COLOR);

	} else {
		res = raycast_sdf(voxel_data, ray_origin, ray_dir, max_distance, 0);
	}

	return res;
}

Ref<VoxelRaycastResult> raycast_generic_world(
		const VoxelData &voxel_data,
		const Ref<VoxelMesher> mesher,
		const Transform3D &to_world,
		const Vector3 ray_origin_world,
		const Vector3 ray_dir_world,
		const float max_distance_world,
		const uint32_t p_collision_mask,
		const uint8_t binary_search_iterations
) {
	// TODO Implement broad-phase on blocks to minimize locking and increase performance

	// TODO Optimization: voxel raycast uses `get_voxel` which is the slowest, but could be made faster.
	// See `VoxelToolLodTerrain` for information about how to implement improvements.

	// TODO Switch to "from/to" parameters instead of "from/dir/distance"

	const Vector3 ray_end_world = ray_origin_world + ray_dir_world * max_distance_world;

	const Transform3D to_local = to_world.affine_inverse();

	const Vector3 pos0_local = to_local.xform(ray_origin_world);
	const Vector3 pos1_local = to_local.xform(ray_end_world);

	const float max_distance_local_sq = pos0_local.distance_squared_to(pos1_local);
	if (max_distance_local_sq < 0.000001f) {
		return Ref<VoxelRaycastResult>();
	}
	const float max_distance_local = Math::sqrt(max_distance_local_sq);
	const Vector3 dir_local = (pos1_local - pos0_local) / max_distance_local;

	Ref<VoxelRaycastResult> res =
			raycast_generic(voxel_data, mesher, pos0_local, dir_local, max_distance_local, p_collision_mask, 0);

	if (res.is_valid()) {
		const float max_distance_world_sq = ray_origin_world.distance_squared_to(ray_end_world);
		const float to_world_scale = max_distance_world_sq / max_distance_local_sq;

		res->distance_along_ray = res->distance_along_ray * to_world_scale;
	}

	return res;
}

} // namespace zylann::voxel