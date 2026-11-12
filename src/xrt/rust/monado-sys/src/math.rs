// Copyright 2026, UltraDev.
// SPDX-License-Identifier: BSL-1.0

use crate::{math_matrix_4x4_isometry_from_pose, math_pose_from_isometry, math_pose_invert, math_pose_transform, math_pose_transform_point, math_quat_exp_so3, math_quat_from_matrix_3x3, math_quat_normalize, math_quat_rotate_vec3, xrt_matrix_3x3, xrt_matrix_4x4, xrt_pose, xrt_quat, xrt_vec3, xrt_vec3_f64};

impl From<[f32; 3]> for xrt_vec3 {
    fn from(value: [f32; 3]) -> Self {
        Self {
            x: value[0],
            y: value[1],
            z: value[2],
        }
    }
}

impl From<[f64; 3]> for xrt_vec3 {
    fn from(value: [f64; 3]) -> Self {
        Self {
            x: value[0] as f32,
            y: value[1] as f32,
            z: value[2] as f32,
        }
    }
}

impl From<xrt_vec3> for xrt_vec3_f64 {
    fn from(value: xrt_vec3) -> Self {
        Self {
            x: value.x as f64,
            y: value.y as f64,
            z: value.z as f64,
        }
    }
}

impl From<[f32; 3]> for xrt_vec3_f64 {
    fn from(value: [f32; 3]) -> Self {
        Self {
            x: value[0] as f64,
            y: value[1] as f64,
            z: value[2] as f64,
        }
    }
}

impl From<[f64; 3]> for xrt_vec3_f64 {
    fn from(value: [f64; 3]) -> Self {
        Self {
            x: value[0],
            y: value[1],
            z: value[2],
        }
    }
}

pub const XRT_QUAT_IDENTITY: xrt_quat = xrt_quat { x: 0., y: 0., z: 0., w: 1. };
pub const XRT_VEC3_ZERO: xrt_vec3 = xrt_vec3 { x: 0., y: 0., z: 0. };
pub const XRT_POSE_IDENTITY: xrt_pose = xrt_pose { orientation: XRT_QUAT_IDENTITY, position: XRT_VEC3_ZERO };

impl Default for xrt_quat {
    fn default() -> Self {
        XRT_QUAT_IDENTITY
    }
}

pub fn quat_rotate_vec3(q: &xrt_quat, v: &xrt_vec3) -> xrt_vec3 {
    let mut out = xrt_vec3::default();
    unsafe { math_quat_rotate_vec3(q, v, &mut out) };
    out
}

/// `a * b`: `b` in `a`'s frame.
pub fn pose_transform(a: &xrt_pose, b: &xrt_pose) -> xrt_pose {
    let mut out = xrt_pose::default();
    unsafe { math_pose_transform(a, b, &mut out) };
    out
}

pub fn pose_transform_point(pose: &xrt_pose, point: &xrt_vec3) -> xrt_vec3 {
    let mut out = xrt_vec3::default();
    unsafe { math_pose_transform_point(pose, point, &mut out) };
    out
}

pub fn pose_invert(pose: &xrt_pose) -> xrt_pose {
    let mut out = xrt_pose::default();
    unsafe { math_pose_invert(pose, &mut out) };
    out
}

/// `r` is row-major.
pub fn pose_from_rt(r: [[f64; 3]; 3], t: [f64; 3]) -> xrt_pose {
    let mat = xrt_matrix_3x3 { v: std::array::from_fn(|i| r[i / 3][i % 3] as f32) };
    let mut orientation = xrt_quat::default();
    unsafe {
        math_quat_from_matrix_3x3(&mat, &mut orientation);
        // Eigen does not renormalise, and the matrices come from files a few digits wide.
        math_quat_normalize(&mut orientation);
    }
    xrt_pose { orientation, position: t.into() }
}

pub fn pose_from_rotation_vector(om: [f64; 3], t: [f64; 3]) -> xrt_pose {
    let mut orientation = xrt_quat::default();
    unsafe { math_quat_exp_so3(&om.into(), &mut orientation) };
    xrt_pose { orientation, position: t.into() }
}

pub fn pose_from_isometry(isometry: xrt_matrix_4x4) -> xrt_pose {
    let mut out = xrt_pose::default();
    unsafe { math_pose_from_isometry(&isometry, &mut out) };
    out
}

/// Column-major, as `t_slam_camera_calibration::T_imu_cam` requires.
pub fn matrix_4x4_from_pose(pose: &xrt_pose) -> xrt_matrix_4x4 {
    let mut out = xrt_matrix_4x4::default();
    unsafe { math_matrix_4x4_isometry_from_pose(pose, &mut out) };
    out
}