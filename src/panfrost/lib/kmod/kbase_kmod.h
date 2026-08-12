/* SPDX-License-Identifier: MIT */

#pragma once

#include "pan_kmod.h"

struct kbase_syncobj;

extern const struct pan_kmod_ops kbase_kmod_ops;

bool pan_kmod_kbase_fd_matches(int fd);
bool pan_kmod_dev_is_kbase(const struct pan_kmod_dev *dev);

struct kbase_syncobj *
pan_kmod_kbase_syncobj_create(struct pan_kmod_dev *dev);
void pan_kmod_kbase_syncobj_destroy(struct pan_kmod_dev *dev,
                                    struct kbase_syncobj *sync);
struct kbase_syncobj *
pan_kmod_kbase_syncobj_dup(struct pan_kmod_dev *dev,
                           struct kbase_syncobj *sync);
bool pan_kmod_kbase_syncobj_wait(struct pan_kmod_dev *dev,
                                 struct kbase_syncobj *sync,
                                 int64_t timeout_ns);

int pan_kmod_kbase_jm_submit(struct pan_kmod_dev *dev, uint64_t jc,
                             uint32_t requirements,
                             struct kbase_syncobj *out_sync,
                             int32_t *bo_handles, uint32_t bo_handle_count);
