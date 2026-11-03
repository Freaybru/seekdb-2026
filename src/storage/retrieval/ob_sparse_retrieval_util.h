/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef OB_SPARSE_RETRIEVAL_UTIL_H_
#define OB_SPARSE_RETRIEVAL_UTIL_H_

#include "common/datum/ob_datum.h"
#include "share/datum/ob_datum_funcs.h"

namespace oceanbase
{
namespace storage
{

// Fast exact comparison for fixed 16-byte binary domain ids (e.g. doc id).
// Only valid when the column's compare func is the binary-collation string
// comparison: memcmp over the common prefix, then length. For two plain
// (non-null, non-ext, inline) 16-byte datums this is a big-endian compare of
// two 64-bit words. Returns false when the fast path does not apply.
OB_INLINE bool ob_sr_fast_bin16_cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret)
{
  bool done = false;
  if (16 == l.pack_ && 16 == r.pack_) {
    uint64_t a = 0, b = 0;
    MEMCPY(&a, l.ptr_, 8);
    MEMCPY(&b, r.ptr_, 8);
    if (a == b) {
      MEMCPY(&a, l.ptr_ + 8, 8);
      MEMCPY(&b, r.ptr_ + 8, 8);
    }
    if (a == b) {
      cmp_ret = 0;
    } else {
      a = __builtin_bswap64(a);
      b = __builtin_bswap64(b);
      cmp_ret = a < b ? -1 : 1;
    }
    done = true;
  }
  return done;
}

OB_INLINE bool ob_sr_is_bin_str_id(const ObObjType type, const ObCollationType cs_type)
{
  return CS_TYPE_BINARY == cs_type && ob_is_string_tc(type);
}

class ObDomainIdCmp
{
public:
  ObDomainIdCmp() : cmp_func_(nullptr), fast_bin_(false) {}
  ~ObDomainIdCmp() {}
  inline int init(const ObObjMeta &obj_meta);
  inline int compare(const ObDatum &lhs, const ObDatum &rhs, int &cmp_ret) const;
  inline void reset() { cmp_func_ = nullptr; fast_bin_ = false; }
private:
  common::ObDatumCmpFuncType cmp_func_;
  bool fast_bin_;
  DISALLOW_COPY_AND_ASSIGN(ObDomainIdCmp);
};

int ObDomainIdCmp::init(const ObObjMeta &obj_meta)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!obj_meta.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN,"invalid argument", K(ret), K(obj_meta));
  } else {
    common::ObDatumBasicFuncs *id_basic_funcs = ObDatumFuncs::get_basic_func(
      obj_meta.get_type(), obj_meta.get_collation_type());
    if (OB_ISNULL(id_basic_funcs)) {
      ret = OB_ERR_UNEXPECTED;
      STORAGE_LOG(WARN,"failed to get basic functions", K(ret), K(obj_meta));
    } else {
      cmp_func_ = id_basic_funcs->null_first_cmp_;
      fast_bin_ = ob_sr_is_bin_str_id(obj_meta.get_type(), obj_meta.get_collation_type());
    }
  }
  return ret;
}

int ObDomainIdCmp::compare(const ObDatum &lhs, const ObDatum &rhs, int &cmp_ret) const
{
  int ret = OB_SUCCESS;
  if (OB_LIKELY(fast_bin_) && ob_sr_fast_bin16_cmp(lhs, rhs, cmp_ret)) {
    return ret;
  }
  const bool lmax = lhs.is_max();
  const bool lmin = lhs.is_min();
  const bool rmax = rhs.is_max();
  const bool rmin = rhs.is_min();
  if (OB_UNLIKELY(nullptr == cmp_func_)) {
    ret = OB_NOT_INIT;
    STORAGE_LOG(WARN,"not init", K(ret));
  } else if (lhs.is_ext() || rhs.is_ext()) {
    if (OB_UNLIKELY(!(lmax || lmin || rmax || rmin))) {
      ret = OB_ERR_UNEXPECTED;
      STORAGE_LOG(WARN,"unexpected ext datum", K(ret), K(lhs), K(rhs));
    } else if (lmax || rmax) {
      cmp_ret = lmax - rmax;
    } else if (lmin || rmin) {
      cmp_ret = -(lmin - rmin);
    } else {
      ret = OB_ERR_UNEXPECTED;
    }
  } else {
    ret = cmp_func_(lhs, rhs, cmp_ret, nullptr);
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase

#endif
