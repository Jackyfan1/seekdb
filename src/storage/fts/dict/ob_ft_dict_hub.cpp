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

#define USING_LOG_PREFIX STORAGE_FTS

#include "storage/fts/dict/ob_ft_dict_hub.h"

#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "storage/fts/dict/ob_ft_cache_container.h"
#include "storage/fts/dict/ob_ft_dict.h"
#include "storage/fts/dict/ob_ft_dict_def.h"
#include "storage/fts/dict/ob_ft_range_dict.h"
namespace oceanbase
{
namespace storage
{
int ObFTDictHub::init()
{
  static constexpr int K_MAX_DICT_BUCKET = 128; // for now, only built-in dicts.
  int ret = OB_SUCCESS;
  if (OB_FAIL(dict_map_.create(K_MAX_DICT_BUCKET, "dict_map"))) {
  } else if (OB_FAIL(dict_object_map_.create(16, "dict_obj_map"))) {
  } else if (OB_FAIL(rw_dict_lock_.init(K_MAX_DICT_BUCKET))) {
  } else {
    is_inited_ = true;
  }
  return ret;
};

int ObFTDictHub::destroy()
{
  int ret = OB_SUCCESS;
  is_inited_ = false;
  // release the shared dictionary objects and the cache blocks they pin
  if (dict_object_map_.created()) {
    for (hash::ObHashMap<ObFTDictInfoKey, ObIFTDict *>::iterator iter = dict_object_map_.begin();
         iter != dict_object_map_.end();
         ++iter) {
      ObIFTDict *dict = iter->second;
      if (OB_NOT_NULL(dict)) {
        dict->~ObIFTDict();
      }
    }
    dict_object_map_.destroy();
  }
  for (int64_t i = 0; i < cached_containers_.count(); ++i) {
    if (OB_NOT_NULL(cached_containers_.at(i))) {
      cached_containers_.at(i)->reset();
      cached_containers_.at(i)->~ObFTCacheRangeContainer();
    }
  }
  cached_containers_.reset();
  dict_object_alloc_.reset();
  dict_map_.destroy();
  return ret;
}
int ObFTDictHub::build_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  ObFTDictInfo info;
  container.reset();

  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    ObBucketHashWLockGuard guard(rw_dict_lock_, key.hash());

    // try if valid with no recursive lock
    if (OB_FAIL(get_dict_info(key, info))) {
      if (OB_HASH_NOT_EXIST == ret) {
        // dict not exist, make new one, by caller
        ret = OB_ENTRY_NOT_EXIST;
      } else {
      }
    } else if (OB_FAIL(ObFTRangeDict::try_load_cache(desc, info.range_count_, container))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
      } else {
      }
    }

    if (OB_FAIL(ret)) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        if (OB_FAIL(ObFTRangeDict::build_cache_from_ik_dict(desc, container))) {
        } else if (FALSE_IT(info.range_count_ = container.get_handles().size())) {
        } else if (OB_FAIL(put_dict_info(key, info))) {
        } else {
          // the dictionary content changed, drop the shared dict object
          remove_dict_object(key);
        }
      }
    }
  }
  return ret;
}

void ObFTDictHub::remove_dict_object(const ObFTDictInfoKey &key)
{
  ObIFTDict *dict = nullptr;
  if (OB_SUCCESS == dict_object_map_.get_refactored(key, dict) && OB_NOT_NULL(dict)) {
    dict->~ObIFTDict();
    int tmp_ret = dict_object_map_.erase_refactored(key);
    UNUSED(tmp_ret);
  }
}

int ObFTDictHub::get_dict_object(const ObFTDictDesc &desc, ObIFTDict *&dict)
{
  int ret = OB_SUCCESS;
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  dict = nullptr;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    ObBucketHashWLockGuard guard(rw_dict_lock_, key.hash());
    if (OB_FAIL(dict_object_map_.get_refactored(key, dict))) {
      if (OB_HASH_NOT_EXIST == ret) {
        ret = OB_SUCCESS;
      }
    }
    if (OB_SUCC(ret) && OB_NOT_NULL(dict)) {
      // shared dictionary is ready
    } else if (OB_FAIL(ret)) {
    } else {
      ObFTDictInfo info;
      ObFTCacheRangeContainer *container = nullptr;
      void *mem = nullptr;
      if (OB_FAIL(get_dict_info(key, info))) {
        if (OB_HASH_NOT_EXIST == ret) {
          ret = OB_SUCCESS;
          info.range_count_ = 0;
        }
      }
      if (OB_SUCC(ret)) {
        if (OB_ISNULL(mem = dict_object_alloc_.alloc(sizeof(ObFTCacheRangeContainer)))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          container = new (mem) ObFTCacheRangeContainer(dict_object_alloc_);
        }
      }
      // read the ranges from the cache, build them if the cache is missing
      if (OB_SUCC(ret) && info.range_count_ > 0) {
        if (OB_FAIL(ObFTRangeDict::try_load_cache(desc, info.range_count_, *container))) {
          if (OB_ENTRY_NOT_EXIST == ret) {
            ret = OB_SUCCESS;
          }
        }
      }
      if (OB_SUCC(ret) && container->get_handles().size() == 0) {
        if (OB_FAIL(ObFTRangeDict::build_cache_from_ik_dict(desc, *container))) {
        } else if (FALSE_IT(info.range_count_ = container->get_handles().size())) {
        } else if (OB_FAIL(put_dict_info(key, info))) {
        }
      }
      if (OB_SUCC(ret)) {
        ObFTRangeDict *range_dict = OB_NEWx(ObFTRangeDict,
                                           &dict_object_alloc_,
                                           dict_object_alloc_,
                                           container,
                                           desc);
        if (OB_ISNULL(range_dict)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else if (OB_FAIL(range_dict->init())) {
          OB_DELETEx(ObFTRangeDict, &dict_object_alloc_, range_dict);
        } else if (OB_FAIL(cached_containers_.push_back(container))) {
          OB_DELETEx(ObFTRangeDict, &dict_object_alloc_, range_dict);
        } else if (OB_FAIL(dict_object_map_.set_refactored(key, static_cast<ObIFTDict *>(range_dict), 1))) {
        } else {
          dict = range_dict;
        }
      }
      if (OB_FAIL(ret) && OB_NOT_NULL(container)) {
        container->reset();
        container->~ObFTCacheRangeContainer();
        container = nullptr;
      }
    }
  }
  return ret;
}

int ObFTDictHub::load_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  ObFTDictInfo info;
  container.reset();
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    {
      ObBucketHashRLockGuard guard(rw_dict_lock_, key.hash());
      if (OB_FAIL(get_dict_info(key, info))) {
        if (OB_HASH_NOT_EXIST == ret) {
          // dict not exist, make new one, by caller
          ret = OB_ENTRY_NOT_EXIST;
        } else {
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObFTRangeDict::try_load_cache(desc, info.range_count_, container))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        // dict not exist, make new one, by caller
      } else {
      }
    }
  }

  return ret;
}


int ObFTDictHub::get_dict_info(const ObFTDictInfoKey &key, ObFTDictInfo &info)
{
  int ret = OB_SUCCESS;

  if (OB_FAIL(dict_map_.get_refactored(key, info))) {
    if (OB_HASH_NOT_EXIST != ret) {
    }
  }

  return ret;
}

int ObFTDictHub::put_dict_info(const ObFTDictInfoKey &key, const ObFTDictInfo &info)
{
  int ret = OB_SUCCESS;
  const int cover_exist_flag = 1;
  if (OB_FAIL(dict_map_.set_refactored(key, info, cover_exist_flag))) {
  }

  return ret;
}
} //  namespace storage
} //  namespace oceanbase
