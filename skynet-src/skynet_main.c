#include "skynet.h"

#include "skynet_imp.h"
#include "skynet_env.h"
#include "skynet_server.h"
#include "luashrtbl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <signal.h>
#include <assert.h>
#include <pthread.h>

static int
optint(const char *key, int opt) {
	const char * str = skynet_getenv(key);
	if (str == NULL) {
		char tmp[20];
		sprintf(tmp,"%d",opt);
		skynet_setenv(key, tmp);
		return opt;
	}
	return strtol(str, NULL, 10);
}

static int
optboolean(const char *key, int opt) {
	const char * str = skynet_getenv(key);
	if (str == NULL) {
		skynet_setenv(key, opt ? "true" : "false");
		return opt;
	}
	return strcmp(str,"true")==0;
}

static const char *
optstring(const char *key,const char * opt) {
	const char * str = skynet_getenv(key);
	if (str == NULL) {
		if (opt) {
			skynet_setenv(key, opt);
			opt = skynet_getenv(key);
		}
		return opt;
	}
	return str;
}

static void
_init_env(lua_State *L) {
	lua_pushnil(L);  /* first key */
	while (lua_next(L, -2) != 0) {
		int keyt = lua_type(L, -2);
		if (keyt != LUA_TSTRING) {
			fprintf(stderr, "Invalid config table\n");
			exit(1);
		}
		const char * key = lua_tostring(L,-2);
		if (lua_type(L,-1) == LUA_TBOOLEAN) {
			int b = lua_toboolean(L,-1);
			skynet_setenv(key,b ? "true" : "false" );
		} else {
			const char * value = lua_tostring(L,-1);
			if (value == NULL) {
				fprintf(stderr, "Invalid config table key = %s\n", key);
				exit(1);
			}
			skynet_setenv(key,value);
		}
		lua_pop(L,1);
	}
	lua_pop(L,1);
}

int sigign() {
	struct sigaction sa;
	sa.sa_handler = SIG_IGN;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGPIPE, &sa, 0);
	return 0;
}

static const char * load_config = "\
	local result = {}\n\
	local config_name, env = ...\n\
	local function getenv(name) return assert(env[name] or os.getenv(name), [[os.getenv() failed: ]] .. name) end\n\
	local sep = package.config:sub(1,1)\n\
	local current_path = [[.]]..sep\n\
	local function include(filename)\n\
		local last_path = current_path\n\
		local path, name = filename:match([[(.*]]..sep..[[)(.*)$]])\n\
		if path then\n\
			if path:sub(1,1) == sep then	-- root\n\
				current_path = path\n\
			else\n\
				current_path = current_path .. path\n\
			end\n\
		else\n\
			name = filename\n\
		end\n\
		local f = assert(io.open(current_path .. name))\n\
		local code = assert(f:read [[*a]])\n\
		code = string.gsub(code, [[%$([%w_%d]+)]], getenv)\n\
		f:close()\n\
		assert(load(code,[[@]]..filename,[[t]],result))()\n\
		current_path = last_path\n\
	end\n\
	setmetatable(result, { __index = { include = include } })\n\
	include(config_name)\n\
	setmetatable(result, nil)\n\
	return result\n\
";


static void *
thread_main(void *p) {
	skynet_start((struct skynet_config *)p);
	skynet_globalexit();
	luaS_exitshr();
	return NULL;
}

LUALIB_API int lmain(lua_State *_L) {
	const char * config_file = luaL_checkstring(_L, 1);

	luaS_initshr();
	skynet_globalinit();
	skynet_env_init();

	struct skynet_config *config = malloc(sizeof(struct skynet_config));

	struct lua_State *L = luaL_newstate();
	luaL_openlibs(L);	// link lua lib

	int err =  luaL_loadbufferx(L, load_config, strlen(load_config), "=[skynet config]", "t");
	assert(err == LUA_OK);
	lua_pushstring(L, config_file);
    lua_newtable(L);
    if (lua_gettop(_L) > 1) {
        luaL_checktype(_L, 2, LUA_TTABLE);
        lua_pushnil(_L);
        while (lua_next(_L, 2)) {
            lua_pushvalue(_L, -2);
            lua_pushstring(L, lua_tostring(_L, -1));
            lua_pushstring(L, lua_tostring(_L, -2));
            lua_settable(L, -3);
            lua_pop(_L, 2);
        }
    }

	err = lua_pcall(L, 2, 1, 0);
	if (err) {
		size_t sz = 0;
        const char *error = lua_tolstring(L, -1, &sz);
		lua_pushlstring(_L, error, sz);
		lua_close(L);
		lua_error(_L);
	}
	_init_env(L);

	config->thread =  optint("thread",8);
	config->module_path = optstring("cpath","./cservice/?.so");
	config->harbor = optint("harbor", 1);
	config->bootstrap = optstring("bootstrap","snlua bootstrap");
	config->daemon = optstring("daemon", NULL);
	config->logger = optstring("logger", NULL);
	config->logservice = optstring("logservice", "logger");
	config->profile = optboolean("profile", 1);

	lua_close(L);

	pthread_t pid;
	if (pthread_create(&pid,NULL, thread_main, config)) {
		luaL_error(_L, "Create sncore main thread failed");
	}

	return 0;
}

LUALIB_API int luaopen_lskynet(lua_State *L) {
    luaL_Reg l[] = {
        { "start", lmain },
        { NULL, NULL },
    };
    luaL_checkversion(L);
    luaL_newlib(L, l);
    return 1;
}

