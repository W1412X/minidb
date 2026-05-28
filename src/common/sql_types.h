/**
 * @file sql_types.h
 * @brief SQL type-name mapping helpers.
 */
#pragma once

#include "container/string.h"
#include "record/value.h"

namespace minidb {

TypeId type_from_sql_name(const String& name);
const char* sql_name_from_type(TypeId type);

} // namespace minidb
