/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Lua Firefox specific cuckoo filter with payload implementation
 *         @file */

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
//#include <time.h> // todo comment out after profiling

#include "lauxlib.h"
#include "lua.h"
#include "xxhash.h"

#ifdef LUA_SANDBOX
#include "../luasandbox_output.h"
#include "../luasandbox_serialize.h"
#endif

static const char* mozsvc_fxcf = "mozsvc.fxcf";

#define BUCKET_SIZE 4

typedef struct fx_data {
  unsigned char country;
  unsigned char channel :3;
  unsigned char os :2;
  unsigned char dflt :1; // the default setting as of the last submission
  unsigned char reserved :2;
  unsigned char dow; // day of the week the 8th bit is the new flag
} fx_data;

typedef struct fxcf_bucket
{
  unsigned short entries[BUCKET_SIZE];
  fx_data data[BUCKET_SIZE];
} fxcf_bucket;

typedef struct fxcf
{
  size_t items;
  size_t bytes;
  size_t num_buckets;
  size_t cnt;
  int nlz;
  fxcf_bucket buckets[];
} fxcf;


/**
 * Hacker's Delight - Henry S. Warren, Jr. page 48
 *
 * @param x
 *
 * @return unsigned Least power of 2 greater than or equal to x
 */
static unsigned clp2(unsigned x)
{
  x = x - 1;
  x = x | (x >> 1);
  x = x | (x >> 2);
  x = x | (x >> 4);
  x = x | (x >> 8);
  x = x | (x >> 16);
  return x + 1;
}


/**
 * Hacker's Delight - Henry S. Warren, Jr. page 78
 *
 * @param x
 *
 * @return int Number of leading zeros
 */
static int nlz(unsigned x)
{
  int n;

  if (x == 0) return 32;
  n = 1;
  if ((x >> 16) == 0) {n = n + 16; x = x << 16;}
  if ((x >> 24) == 0) {n = n + 8; x = x << 8;}
  if ((x >> 28) == 0) {n = n + 4; x = x << 4;}
  if ((x >> 30) == 0) {n = n + 2; x = x << 2;}
  n = n - (x >> 31);
  return n;
}


static int fxcf_new(lua_State* lua)
{
  int n = lua_gettop(lua);
  luaL_argcheck(lua, n == 1, 0, "incorrect number of arguments");
  int items = luaL_checkint(lua, 1);
  luaL_argcheck(lua, 4 < items, 1, "items must be > 4");

  unsigned buckets = clp2((unsigned)ceil(items / BUCKET_SIZE));
  size_t bytes = sizeof(fxcf_bucket) * buckets;
  size_t nbytes = sizeof(fxcf) + bytes;
  fxcf* cf = (fxcf*)lua_newuserdata(lua, nbytes);
  cf->items = buckets * BUCKET_SIZE;
  cf->num_buckets = buckets;
  cf->bytes = bytes;
  cf->cnt = 0;
  cf->nlz = nlz(buckets) + 1;
  memset(cf->buckets, 0, cf->bytes);

  luaL_getmetatable(lua, mozsvc_fxcf);
  lua_setmetatable(lua, -2);
  return 1;
}


static fxcf* check_fxcf(lua_State* lua, int args)
{
  void* ud = luaL_checkudata(lua, 1, mozsvc_fxcf);
  luaL_argcheck(lua, ud != NULL, 1, "invalid userdata type");
  luaL_argcheck(lua, args == lua_gettop(lua), 0,
                "incorrect number of arguments");
  return (fxcf*)ud;
}


static unsigned fingerprint(unsigned h)
{
  h = h >> 16;
  return h ? h : 1;
}


static bool bucket_query_lookup(fxcf_bucket* b, unsigned fp, const fx_data* data)
{
  for (int i = 0; i < BUCKET_SIZE; ++i) {
    if (b->entries[i] == fp) {
      return true;
    }
  }
  return false;
}


static bool bucket_insert_lookup(fxcf_bucket* b, unsigned fp, const fx_data* data)
{
  for (int i = 0; i < BUCKET_SIZE; ++i) {
    if (b->entries[i] == fp) {
      if (data) {
        b->data[i].country = data->country;
        b->data[i].channel = data->channel;
        b->data[i].os = data->os; // os should never change for a clientId
        b->data[i].dow |= data->dow;
      }
      return true;
    }
  }
  return false;
}


static bool bucket_delete(fxcf_bucket* b, unsigned fp)
{
  for (int i = 0; i < BUCKET_SIZE; ++i) {
    if (b->entries[i] == fp) {
      b->entries[i] = 0;
      memset(&b->data[i], 0, sizeof(fx_data));
      return true;
    }
  }
  return false;
}


static bool bucket_add(fxcf_bucket* b, unsigned fp, const fx_data* data)
{
  for (int i = 0; i < BUCKET_SIZE; ++i) {
    if (b->entries[i] == 0) {
      b->entries[i] = fp;
      b->data[i] = *data;
      b->data[i].dow |= 128; // set the new flag
      return true;
    }
  }
  return false;
}


static bool bucket_insert(fxcf* cf, unsigned i1, unsigned i2,
                          unsigned fp, const fx_data* data)
{
  // since we must handle duplicates we consider any collision within the bucket
  // to be a duplicate. The 16 bit fingerprint makes the false postive rate very
  // low 0.00012
  if (bucket_insert_lookup(&cf->buckets[i1], fp, data)) return false;
  if (bucket_insert_lookup(&cf->buckets[i2], fp, data)) return false;

  if (!bucket_add(&cf->buckets[i1], fp, data)) {
    if (!bucket_add(&cf->buckets[i2], fp, data)) {
      unsigned ri;
      if (rand() % 2) {
        ri = i1;
      } else {
        ri = i2;
      }
      fx_data tmpdata;
      for (int i = 0; i < 512; ++i) {
        int entry = rand() % BUCKET_SIZE;
        unsigned tmp = cf->buckets[ri].entries[entry];
        tmpdata = cf->buckets[ri].data[entry];
        cf->buckets[ri].entries[entry] = fp;
        cf->buckets[ri].data[entry] = *data;
        fp = tmp;
        data = &tmpdata;
        ri = ri ^ (XXH32(&fp, sizeof(unsigned), 1) >> cf->nlz);
        if (bucket_insert_lookup(&cf->buckets[ri], fp, data)) return false;
        if (bucket_add(&cf->buckets[ri], fp, data)) {
          return true;
        }
      }
      return false;
    }
  }
  return true;
}


static int fxcf_add(lua_State* lua)
{
  fxcf* cf = check_fxcf(lua, 7);
  size_t len = 0;
  double val = 0;
  unsigned country, channel, os, day, dflt;
  void* key = NULL;
  switch (lua_type(lua, 2)) {
  case LUA_TSTRING:
    key = (void*)lua_tolstring(lua, 2, &len);
    break;
  case LUA_TNUMBER:
    val = lua_tonumber(lua, 2);
    len = sizeof(double);
    key = &val;
    break;
  default:
    return luaL_argerror(lua, 2, "must be a string or number");
  }
  if (lua_type(lua, 3) == LUA_TNUMBER) {
    country = (unsigned)lua_tointeger(lua, 3);
    if (country > 255) {
      return luaL_argerror(lua, 3, "must a number 0-255");
    }
  } else {
    return luaL_argerror(lua, 3, "must a number");
  }
  if (lua_type(lua, 4) == LUA_TNUMBER) {
    channel = (unsigned)lua_tointeger(lua, 4);
    if (channel > 7) {
      return luaL_argerror(lua, 4, "must a number 0-7");
    }
  } else {
    return luaL_argerror(lua, 4, "must a number");
  }
  if (lua_type(lua, 5) == LUA_TNUMBER) {
    os = (unsigned)lua_tointeger(lua, 5);
    if (os > 3) {
      return luaL_argerror(lua, 5, "must a number 0-3");
    }
  } else {
    return luaL_argerror(lua, 5, "must a number");
  }
  if (lua_type(lua, 6) == LUA_TNUMBER) {
    day = (unsigned)lua_tointeger(lua, 6);
    if (day > 6) {
      return luaL_argerror(lua, 6, "must a number 0-6");
    }
  } else {
    return luaL_argerror(lua, 6, "must a number");
  }
  if (lua_type(lua, 7) == LUA_TBOOLEAN) {
    dflt = (unsigned)lua_toboolean(lua, 7);
  } else {
    return luaL_argerror(lua, 7, "must a boolean");
  }
  fx_data data;
  data.channel = channel;
  data.country = country;
  data.os = os;
  data.dflt = dflt;
  data.dow = 1 << day;
  unsigned h = XXH32(key, (int)len, 1);
  unsigned fp = fingerprint(h);
  unsigned i1 = h % cf->num_buckets;
  unsigned i2 = i1 ^ (XXH32(&fp, sizeof(unsigned), 1) >> cf->nlz);
  bool success = bucket_insert(cf, i1, i2, fp, &data);
  if (success) {
    ++cf->cnt;
  }
  lua_pushboolean(lua, success);
  return 1;
}


static int fxcf_query(lua_State* lua)
{
  fxcf* cf = check_fxcf(lua, 2);
  size_t len = 0;
  double val = 0;
  void* key = NULL;
  switch (lua_type(lua, 2)) {
  case LUA_TSTRING:
    key = (void*)lua_tolstring(lua, 2, &len);
    break;
  case LUA_TNUMBER:
    val = lua_tonumber(lua, 2);
    len = sizeof(double);
    key = &val;
    break;
  default:
    return luaL_argerror(lua, 2, "must be a string or number");
  }
  unsigned h = XXH32(key, (int)len, 1);
  unsigned fp = fingerprint(h);
  unsigned i1 = h % cf->num_buckets;

  fx_data data;
  bool found = bucket_query_lookup(&cf->buckets[i1], fp, &data);
  if (!found) {
    unsigned i2 = i1 ^ (XXH32(&fp, sizeof(unsigned), 1) >> cf->nlz);
    found = bucket_query_lookup(&cf->buckets[i2], fp, &data);
  }
  lua_pushboolean(lua, found);
  return 1;
}


static int fxcf_delete(lua_State* lua)
{
  fxcf* cf = check_fxcf(lua, 2);
  size_t len = 0;
  double val = 0;
  void* key = NULL;
  switch (lua_type(lua, 2)) {
  case LUA_TSTRING:
    key = (void*)lua_tolstring(lua, 2, &len);
    break;
  case LUA_TNUMBER:
    val = lua_tonumber(lua, 2);
    len = sizeof(double);
    key = &val;
    break;
  default:
    return luaL_argerror(lua, 2, "must be a string or number");
  }
  unsigned h = XXH32(key, (int)len, 1);
  unsigned fp = fingerprint(h);
  unsigned i1 = h % cf->num_buckets;

  bool deleted = bucket_delete(&cf->buckets[i1], fp);
  if (!deleted) {
    unsigned i2 = i1 ^ (XXH32(&fp, sizeof(unsigned), 1) >> cf->nlz);
    deleted = bucket_delete(&cf->buckets[i2], fp);
  }
  if (deleted) {
    --cf->cnt;
  }
  lua_pushboolean(lua, deleted);
  return 1;
}


static int fxcf_count(lua_State* lua)
{
  fxcf* cf = check_fxcf(lua, 1);
  lua_pushnumber(lua, cf->cnt);
  return 1;
}


static int fxcf_clear(lua_State* lua)
{
  fxcf* cf = check_fxcf(lua, 1);
  memset(cf->buckets, 0, cf->bytes);
  cf->cnt = 0;
  return 0;
}


static int fxcf_fromstring(lua_State* lua)
{
  fxcf* cf = check_fxcf(lua, 3);
  cf->cnt = (size_t)luaL_checknumber(lua, 2);
  size_t len = 0;
  const char* values = luaL_checklstring(lua, 3, &len);
  if (len != cf->bytes) {
    return luaL_error(lua, "fromstring() bytes found: %d, expected %d", len,
                      cf->bytes);
  }
  memcpy(cf->buckets, values, len);
  return 0;
}


static void increment_column(lua_State* lua, int col)
{
  double val;
  lua_rawgeti(lua, -1, col);
  val = lua_tonumber(lua, -1);
  lua_pop(lua, 1);
  ++val;
  lua_pushnumber(lua, val);
  lua_rawseti(lua, -2, col);
}


static int fxcf_report(lua_State* lua)
{
  fxcf* cf = check_fxcf(lua, 2);
  if (lua_type(lua, 2) != LUA_TTABLE) {
    return luaL_argerror(lua, 2, "must be a table");
  }

//  clock_t t = clock();
  int fos; // five of seven
  fx_data* data;
  for (int i = 0; i < cf->num_buckets; ++i) {
    for (int j = 0; j < BUCKET_SIZE; ++j) {
      if (cf->buckets[i].entries[j] != 0) {
        fos = 0;
        data = &cf->buckets[i].data[j];

        // lookup entry in lua table based on country, channel, os
        lua_pushfstring(lua, "%d,%d,%d", data->country, data->channel,
                        data->os);
        lua_gettable(lua, 2);
        if (lua_type(lua, -1) != LUA_TTABLE) continue;

        fos += data->dow & 1;
        fos += (data->dow & 2) >> 1;
        fos += (data->dow & 4) >> 2;
        fos += (data->dow & 8) >> 3;
        fos += (data->dow & 16) >> 4;
        fos += (data->dow & 32) >> 5;
        fos += (data->dow & 64) >> 6;
        if (fos) {
          increment_column(lua, 2); // actives
          if (fos >= 5) {
            increment_column(lua, 6); // five of seven
          }
        } else {
          increment_column(lua, 4); // inactives
        }

        if (data->dow & 128) {
          increment_column(lua, 5); // increment new
        }
        increment_column(lua, 7); // increment total
        if (data->dflt) increment_column(lua, 9);
        lua_pop(lua, 1); // remove table
                         // clear day of the week bit flags
        data->dow = 0;
        data->dflt = 0;
      }
    }
  }
//  t = clock() - t;
//  fprintf(stderr, "fxcf_report time: %g\n", (double)t / CLOCKS_PER_SEC);
  return 0;
}


#ifdef LUA_SANDBOX
static int
serialize_fxcf(lua_State* lua)
{
  lsb_output_data* output = (lsb_output_data*)lua_touserdata(lua, -1);
  const char* key = (const char*)lua_touserdata(lua, -2);
  fxcf* cf = (fxcf*)lua_touserdata(lua, -3);
  if (!(output && key && cf)) {
    return 0;
  }
  if (lsb_appendf(output,
                  "if %s == nil then %s = fxcf.new(%u) end\n",
                  key,
                  key,
                  (unsigned)cf->items)) {
    return 1;
  }

  if (lsb_appendf(output, "%s:fromstring(%u, \"", key, (unsigned)cf->cnt)) {
    return 1;
  }
  if (lsb_serialize_binary(cf->buckets, cf->bytes, output)) return 1;
  if (lsb_appends(output, "\")\n", 3)) {
    return 1;
  }
  return 0;
}
#endif


static const struct luaL_reg fxcflib_f[] =
{
  { "new", fxcf_new },
  { NULL, NULL }
};


static const struct luaL_reg fxcflib_m[] =
{
  { "add", fxcf_add },
  { "query", fxcf_query },
  { "delete", fxcf_delete },
  { "count", fxcf_count },
  { "clear", fxcf_clear },
  { "report", fxcf_report },
  { "fromstring", fxcf_fromstring } // used for data restoration
  , { NULL, NULL }
};


int luaopen_fxcf(lua_State* lua)
{
#ifdef LUA_SANDBOX
  lua_newtable(lua);
  lsb_add_serialize_function(lua, serialize_fxcf);
  lua_replace(lua, LUA_ENVIRONINDEX);
#endif
  luaL_newmetatable(lua, mozsvc_fxcf);
  lua_pushvalue(lua, -1);
  lua_setfield(lua, -2, "__index");
  luaL_register(lua, NULL, fxcflib_m);
  luaL_register(lua, "fxcf", fxcflib_f);
  return 1;
}
