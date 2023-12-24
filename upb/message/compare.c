// Protocol Buffers - Google's data interchange format
// Copyright 2023 Google LLC.  All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "upb/message/compare.h"

#include <string.h>

#include "upb/base/descriptor_constants.h"
#include "upb/base/string_view.h"
#include "upb/mem/arena.h"
#include "upb/message/accessors.h"
#include "upb/message/array.h"
#include "upb/message/internal/accessors.h"
#include "upb/message/internal/compare_unknown.h"
#include "upb/message/internal/extension.h"
#include "upb/message/map.h"
#include "upb/message/message.h"
#include "upb/mini_table/extension.h"
#include "upb/mini_table/field.h"
#include "upb/mini_table/internal/field.h"
#include "upb/mini_table/message.h"
#include "upb/wire/encode.h"

// Must be last.
#include "upb/port/def.inc"

bool upb_Message_NextBaseField(const upb_Message* msg, const upb_MiniTable* m,
                               const upb_MiniTableField** out_f,
                               upb_MessageValue* out_v, size_t* iter) {
  const size_t count = upb_MiniTable_FieldCount(m);
  size_t i = *iter;

  while (++i < count) {
    const upb_MiniTableField* f = upb_MiniTable_GetFieldByIndex(m, i);
    const void* src = UPB_PRIVATE(_upb_Message_DataPtr)(msg, f);

    upb_MessageValue val;
    UPB_PRIVATE(_upb_MiniTableField_DataCopy)(f, &val, src);

    // Skip field if unset or empty.
    if (upb_MiniTableField_HasPresence(f)) {
      if (!upb_Message_HasBaseField(msg, f)) continue;
    } else {
      if (UPB_PRIVATE(_upb_MiniTableField_DataIsZero)(f, src)) continue;

      if (upb_MiniTableField_IsArray(f)) {
        if (upb_Array_Size(val.array_val) == 0) continue;
      } else if (upb_MiniTableField_IsMap(f)) {
        if (upb_Map_Size(val.map_val) == 0) continue;
      }
    }

    *out_f = f;
    *out_v = val;
    *iter = i;
    return true;
  }

  return false;
}

bool upb_Message_NextExtension(const upb_Message* msg, const upb_MiniTable* m,
                               const upb_MiniTableExtension** out_e,
                               upb_MessageValue* out_v, size_t* iter) {
  size_t count;
  const upb_Extension* exts = UPB_PRIVATE(_upb_Message_Getexts)(msg, &count);
  size_t i = *iter;

  if (++i < count) {
    *out_e = exts[i].ext;
    memcpy(out_v, &exts[i].data, sizeof(upb_MessageValue));
    *iter = i;
    return true;
  }

  return false;
}

bool upb_Message_IsEmpty(const upb_Message* msg, const upb_MiniTable* m) {
  if (upb_Message_ExtensionCount(msg)) return false;

  const upb_MiniTableField* f;
  upb_MessageValue v;
  size_t iter = kUpb_BaseField_Begin;
  return !upb_Message_NextBaseField(msg, m, &f, &v, &iter);
}

static bool _upb_IsStringView(const upb_MiniTableField* f) {
  const upb_CType ctype = upb_MiniTableField_CType(f);
  return (ctype == kUpb_CType_String) || (ctype == kUpb_CType_Bytes);
}

static bool _upb_Array_IsEqual(const upb_Array* arr1, const upb_Array* arr2,
                               const upb_MiniTable* m,
                               const upb_MiniTableField* f) {
  // Check for trivial equality.
  if (arr1 == arr2) return true;

  // Must have identical element counts.
  const size_t size1 = arr1 ? upb_Array_Size(arr1) : 0;
  const size_t size2 = arr2 ? upb_Array_Size(arr2) : 0;
  if (size1 != size2) return false;

  // Case #1: Arrays of messages.
  if (upb_MiniTableField_IsSubMessage(f)) {
    const upb_MiniTable* m2 = upb_MiniTable_GetSubMessageTable(m, f);

    for (size_t i = 0; i < size1; i++) {
      const upb_MessageValue val1 = upb_Array_Get(arr1, i);
      const upb_MessageValue val2 = upb_Array_Get(arr2, i);

      if (!upb_Message_IsEqual(val1.msg_val, val2.msg_val, m2)) return false;
    }
    return true;
  }

  // Case #2: Arrays of strings or bytes.
  if (_upb_IsStringView(f)) {
    for (size_t i = 0; i < size1; i++) {
      const upb_MessageValue val1 = upb_Array_Get(arr1, i);
      const upb_MessageValue val2 = upb_Array_Get(arr2, i);

      if (!upb_StringView_IsEqual(val1.str_val, val2.str_val)) return false;
    }
    return true;
  }

  // Case #3: Arrays of scalars.
  const size_t len = 1U << UPB_PRIVATE(_upb_Array_ElemSizeLg2)(arr1);
  for (size_t i = 0; i < size1; i++) {
    const upb_MessageValue val1 = upb_Array_Get(arr1, i);
    const upb_MessageValue val2 = upb_Array_Get(arr2, i);

    if (memcmp(&val1, &val2, len)) return false;
  }
  return true;
}

static bool _upb_Map_IsEqual(const upb_Map* map1, const upb_Map* map2,
                             const upb_MiniTable* m,
                             const upb_MiniTableField* f) {
  // Check for trivial equality.
  if (map1 == map2) return true;

  // Must have identical element counts.
  size_t size1 = map1 ? upb_Map_Size(map1) : 0;
  size_t size2 = map2 ? upb_Map_Size(map2) : 0;
  if (size1 != size2) return false;

  const upb_MiniTable* m2 = upb_MiniTable_GetSubMessageTable(m, f);
  const upb_MiniTableField* f2 = upb_MiniTable_MapValue(m2);
  upb_MessageValue key, val1, val2;

  // Case #1: Maps of messages.
  if (upb_MiniTableField_IsSubMessage(f2)) {
    const upb_MiniTable* m3 = upb_MiniTable_GetSubMessageTable(m2, f2);

    size_t iter = kUpb_Map_Begin;
    while (upb_Map_Next(map1, &key, &val1, &iter)) {
      if (!upb_Map_Get(map2, key, &val2)) return false;

      if (!upb_Message_IsEqual(val1.msg_val, val2.msg_val, m3)) return false;
    }
    return true;
  }

  // Case #2: Maps of strings or bytes.
  if (_upb_IsStringView(f2)) {
    size_t iter = kUpb_Map_Begin;
    while (upb_Map_Next(map1, &key, &val1, &iter)) {
      if (!upb_Map_Get(map2, key, &val2)) return false;

      if (!upb_StringView_IsEqual(val1.str_val, val2.str_val)) return false;
    }
    return true;
  }

  // Case #3: Maps of scalars.
  size_t iter = kUpb_Map_Begin;
  while (upb_Map_Next(map1, &key, &val1, &iter)) {
    if (!upb_Map_Get(map2, key, &val2)) return false;

    if (!UPB_PRIVATE(_upb_MiniTableField_DataEquals)(f2, &val1, &val2)) {
      return false;
    }
  }
  return true;
}

static bool _upb_Message_BaseFieldsAreEqual(const upb_Message* msg1,
                                            const upb_Message* msg2,
                                            const upb_MiniTable* m) {
  // Iterate over all base fields for each message.
  // The order will always match if the messages are equal.
  size_t iter1 = kUpb_BaseField_Begin;
  size_t iter2 = kUpb_BaseField_Begin;

  for (;;) {
    const upb_MiniTableField *f1, *f2;
    upb_MessageValue val1, val2;

    const bool got1 = upb_Message_NextBaseField(msg1, m, &f1, &val1, &iter1);
    const bool got2 = upb_Message_NextBaseField(msg2, m, &f2, &val2, &iter2);

    if (got1 != got2) return false;  // Must have identical field counts.
    if (!got1) return true;          // Loop termination condition.
    if (f1 != f2) return false;      // Must have identical minitables.

    bool eq;
    if (upb_MiniTableField_IsArray(f1)) {
      eq = _upb_Array_IsEqual(val1.array_val, val2.array_val, m, f1);
    } else if (upb_MiniTableField_IsMap(f1)) {
      eq = _upb_Map_IsEqual(val1.map_val, val2.map_val, m, f1);
    } else if (upb_MiniTableField_IsSubMessage(f1)) {
      const upb_MiniTable* subm = upb_MiniTable_GetSubMessageTable(m, f1);
      eq = upb_Message_IsEqual(val1.msg_val, val2.msg_val, subm);
    } else {
      eq = UPB_PRIVATE(_upb_MiniTableField_DataEquals)(f1, &val1, &val2);
    }
    if (!eq) return false;
  }
}

static bool _upb_Message_ExtensionsAreEqual(const upb_Message* msg1,
                                            const upb_Message* msg2,
                                            const upb_MiniTable* m) {
  // Must have identical extension counts.
  if (upb_Message_ExtensionCount(msg1) != upb_Message_ExtensionCount(msg2)) {
    return false;
  }

  const upb_MiniTableExtension* e;
  upb_MessageValue val1, val2;

  // Iterate over all extensions for msg1, and search msg2 for each extension.
  size_t iter1 = kUpb_Extension_Begin;
  while (upb_Message_NextExtension(msg1, m, &e, &val1, &iter1)) {
    const upb_Extension* ext2 = UPB_PRIVATE(_upb_Message_Getext)(msg2, e);
    if (!ext2) return false;
    memcpy(&val2, &ext2->data, sizeof(upb_MessageValue));

    const upb_MiniTableField* f = &e->UPB_PRIVATE(field);

    bool eq;
    if (upb_MiniTableField_IsArray(f)) {
      eq = _upb_Array_IsEqual(val1.array_val, val2.array_val, m, f);
    } else if (upb_MiniTableField_IsMap(f)) {
      eq = _upb_Map_IsEqual(val1.map_val, val2.map_val, m, f);
    } else if (upb_MiniTableField_IsSubMessage(f)) {
      const upb_MiniTable* subm = upb_MiniTableExtension_GetSubMessage(e);
      eq = upb_Message_IsEqual(val1.msg_val, val2.msg_val, subm);
    } else {
      eq = UPB_PRIVATE(_upb_MiniTableField_DataEquals)(f, &val1, &val2);
    }
    if (!eq) return false;
  }
  return true;
}

bool upb_Message_IsEqual(const upb_Message* msg1, const upb_Message* msg2,
                         const upb_MiniTable* m) {
  if (UPB_UNLIKELY(msg1 == msg2)) return true;

  if (!_upb_Message_BaseFieldsAreEqual(msg1, msg2, m)) return false;
  if (!_upb_Message_ExtensionsAreEqual(msg1, msg2, m)) return false;

  // Check the unknown fields.
  size_t usize1, usize2;
  const char* uf1 = upb_Message_GetUnknown(msg1, &usize1);
  const char* uf2 = upb_Message_GetUnknown(msg2, &usize2);

  // 100 is arbitrary, we're trying to prevent stack overflow but it's not
  // obvious how deep we should allow here.
  return UPB_PRIVATE(_upb_Message_UnknownFieldsAreEqual)(
             uf1, usize1, uf2, usize2, 100) == kUpb_UnknownCompareResult_Equal;
}

bool upb_Message_IsExactlyEqual(const upb_Message* msg1,
                                const upb_Message* msg2,
                                const upb_MiniTable* m) {
  if (msg1 == msg2) return true;

  int opts = kUpb_EncodeOption_SkipUnknown | kUpb_EncodeOption_Deterministic;
  upb_Arena* a = upb_Arena_New();

  // Compare deterministically serialized payloads with no unknown fields.
  size_t size1, size2;
  char *data1, *data2;
  upb_EncodeStatus status1 = upb_Encode(msg1, m, opts, a, &data1, &size1);
  upb_EncodeStatus status2 = upb_Encode(msg2, m, opts, a, &data2, &size2);

  if (status1 != kUpb_EncodeStatus_Ok || status2 != kUpb_EncodeStatus_Ok) {
    // TODO: How should we fail here? (In Ruby we throw an exception.)
    upb_Arena_Free(a);
    return false;
  }

  const bool ret = (size1 == size2) && (memcmp(data1, data2, size1) == 0);
  upb_Arena_Free(a);
  return ret;
}
