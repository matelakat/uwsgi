#include <uwsgi.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#if LUA_VERSION_NUM < 502
# define luaL_newuwsgilib(L,l) luaL_register(L, "uwsgi",l)
# define lua_rawlen lua_objlen
#else
# define luaL_newuwsgilib(L,l) lua_newtable(L);luaL_setfuncs (L, l, 0);lua_pushvalue(L,-1);lua_setglobal(L,"uwsgi")
#endif

extern struct uwsgi_server uwsgi;

struct uwsgi_lua {
	struct lua_State ***state;
	char *shell;
	char *filename;
	struct uwsgi_string_list *load;
	int gc_freq;
	int gc_full;
} ulua;

#define ULUA_MYWID (uwsgi.mywid-1)
#define ULUA_WORKER_STATE ulua.state[ULUA_MYWID]

struct uwsgi_plugin lua_plugin;

#define lca(L, n)		ulua_check_args(L, __FUNCTION__, n)

static void uwsgi_opt_luashell(char *opt, char *value, void *foobar) {

        uwsgi.honour_stdin = 1;
        if (value) {
                ulua.shell = value;
        }
        else {
                ulua.shell = "";
        }
}


static struct uwsgi_option uwsgi_lua_options[] = {

	{"lua", required_argument, 0, "load lua wsapi app", uwsgi_opt_set_str, &ulua.filename, 0},
	{"lua-load", required_argument, 0, "load a lua file", uwsgi_opt_add_string_list, &ulua.load, 0},
	{"lua-shell", no_argument, 0, "run the lua interactive shell (debug.debug())", uwsgi_opt_luashell, NULL, 0},
	{"luashell", no_argument, 0, "run the lua interactive shell (debug.debug())", uwsgi_opt_luashell, NULL, 0},
	{"lua-gc-freq", no_argument, 0, "set the lua gc frequency (default: 0, runs after every request)", uwsgi_opt_set_int, &ulua.gc_freq, 0},
	{"lua-gc-full", no_argument, 0, "set the lua gc to perform a full garbage-collection cycle (default: 0, gc performs an incremental step of garbage collection)", uwsgi_opt_set_int, &ulua.gc_full, 0},

	{0, 0, 0, 0},

};

static void ulua_check_args(lua_State *L, const char *func, int n) {
	int args = lua_gettop(L);
	char error[1024];
	if (args != n) {
		if (n == 1) {
			snprintf(error, 1024, "uwsgi.%s takes 1 parameter", func+10);
		}
		else {
			snprintf(error, 1024, "uwsgi.%s takes %d parameters", func+10, n);
		}
		lua_pushstring(L, error);
        	lua_error(L);
	}
}

static int uwsgi_api_log(lua_State *L) {
	
	const char *logline ;

	lca(L, 1);

	if (lua_isstring(L, 1)) {
		logline = lua_tostring(L, 1);
                uwsgi_log( "%s\n", logline);
	}

	return 0;
}

static int uwsgi_api_rpc(lua_State *L) {
	uint8_t argc = lua_gettop(L);
	
	if (argc < 2) {
		lua_pushnil(L);
		return 1;
	}
	
	argc-=2;
	
	char **argv = NULL;
	uint16_t *argvs = NULL;
		
	if (argc > 0) {
		uint8_t i;
		argv = (char **) uwsgi_malloc(sizeof(char *)*argc);
		argvs = (uint16_t *) uwsgi_malloc(sizeof(uint16_t)*argc);
		
		for(i = 0; i < argc; i++) {
			argv[i] = (char *) lua_tolstring(L, i + 3, (size_t *) &argvs[i]); 
		}
	}
	
	uint64_t len;
	char *str = uwsgi_do_rpc((char *) lua_tostring(L, 1), (char *) lua_tostring(L, 2), argc, argv, argvs, &len);
	
	if (len == 0) { // fail??
		lua_pushnil(L);
	} else {
		lua_pushlstring(L, str, len);
	}
	
	if (argc > 0) {
		free(argv);
		free(argvs);
	}

	free(str);
	
	return 1;
}

static int uwsgi_api_register_rpc(lua_State *L) {

        uint8_t argc = lua_gettop(L);
	const char *name;
	// a hack for 64bit;
	int func;
	long lfunc;

	if (argc < 2) {
		lua_pushnil(L);
		return 1;
	}

	name = lua_tostring(L, 1);

	lua_pushvalue(L, 2);
        func = luaL_ref(L, LUA_REGISTRYINDEX);

	//uwsgi_log("registered function %d in global table of lua_State %d\n", func, ULUA_MYWID);
	lfunc = func;

        if (uwsgi_register_rpc((char *)name, &lua_plugin, 0, (void *) lfunc)) {
		lua_pushnil(L);
        }
	else {
		lua_pushboolean(L, 1);
	}

	return 1;
}

static int uwsgi_api_cache_set(lua_State *L) {

	uint8_t argc = lua_gettop(L);
        const char *key ;
        const char *value ;
        uint64_t expires = 0;
	size_t vallen;
	size_t keylen;
	const char *cache = NULL;

	if (argc < 2) goto error;

	key = lua_tolstring(L, 1, &keylen);
	value = lua_tolstring(L, 2, &vallen);
	if (argc > 2) {
		expires = lua_tonumber(L, 3);
		if (argc > 3) {
			cache = lua_tostring(L, 4);
		}
	}

        if (!uwsgi_cache_magic_set((char *)key, keylen, (char *)value, vallen, expires, 0, (char *) cache)) {
		lua_pushboolean(L, 1);
		return 1;
	}
error:

	lua_pushnil(L);
	return 1;

}

static int uwsgi_api_cache_update(lua_State *L) {

        uint8_t argc = lua_gettop(L);
        const char *key ;
        const char *value ;
        uint64_t expires = 0;
        size_t vallen;
        size_t keylen;
        const char *cache = NULL;

        if (argc < 2) goto error;

        key = lua_tolstring(L, 1, &keylen);
        value = lua_tolstring(L, 2, &vallen);
        if (argc > 2) {
                expires = lua_tonumber(L, 3);
                if (argc > 3) {
                        cache = lua_tostring(L, 4);
                }
        }       

        if (!uwsgi_cache_magic_set((char *)key, keylen, (char *)value, vallen, expires, UWSGI_CACHE_FLAG_UPDATE, (char *)cache)) {
                lua_pushboolean(L, 1);
                return 1;
        }
error:

        lua_pushnil(L);
        return 1;

}


static int uwsgi_api_register_signal(lua_State *L) {

	int args = lua_gettop(L);
	uint8_t sig;
	long lhandler;
	const char *who;
	
	if (args >= 3) {

		sig = lua_tonumber(L, 1);
		who = lua_tostring(L, 2);
		lua_pushvalue(L, 3);
		lhandler = luaL_ref(L, LUA_REGISTRYINDEX);

		uwsgi_register_signal(sig, (char *)who, (void *) lhandler, 6);
	}

	lua_pushnil(L);
        return 1;
}

static int uwsgi_api_cache_clear(lua_State *L) {

        const char *cache = NULL;
        uint8_t argc = lua_gettop(L);

        if (argc > 0) {
        	cache = lua_tostring(L, 2);
        }
        if (!uwsgi_cache_magic_clear((char *)cache)) {
        	lua_pushboolean(L, 1);
                return 1;
        }

        lua_pushnil(L);
        return 1;

}


static int uwsgi_api_cache_del(lua_State *L) {

        size_t keylen;
        const char *key ;
        const char *cache = NULL;
        uint8_t argc = lua_gettop(L);

        if (argc == 0) goto error;

        if (lua_isstring(L, 1)) {
                // get the key
                key = lua_tolstring(L, 1, &keylen);
                if (argc > 1) {
                        cache = lua_tostring(L, 2);
                }
                if (!uwsgi_cache_magic_del((char *)key, keylen, (char *)cache)) {
                        lua_pushboolean(L, 1);
                        return 1;
                }
        }

error:
        lua_pushnil(L);
        return 1;

}


static int uwsgi_api_cache_exists(lua_State *L) {

        size_t keylen;
        const char *key ;
        const char *cache = NULL;
        uint8_t argc = lua_gettop(L);

        if (argc == 0) goto error;

        if (lua_isstring(L, 1)) {
                // get the key
                key = lua_tolstring(L, 1, &keylen);
                if (argc > 1) {
                        cache = lua_tostring(L, 2);
                }
                if (uwsgi_cache_magic_exists((char *)key, keylen,(char *)cache)) {
			lua_pushboolean(L, 1);
                	return 1;
                }
        }

error:
        lua_pushnil(L);
        return 1;

}

static int uwsgi_api_async_sleep(lua_State *L) {
	uint8_t argc = lua_gettop(L);
        if (argc == 0) goto end;

        struct wsgi_request *wsgi_req = current_wsgi_req();

        int timeout = lua_tonumber(L, 1);

        if (timeout >= 0) {
                async_add_timeout(wsgi_req, timeout);
        }
end:
	lua_pushnil(L);
        return 1;
}

static int uwsgi_api_wait_fd_read(lua_State *L) {
        uint8_t argc = lua_gettop(L);
        if (argc == 0) goto end;

        struct wsgi_request *wsgi_req = current_wsgi_req();

	int fd = lua_tonumber(L, 1);
	int timeout = 0;
	if (argc > 1) {
        	timeout = lua_tonumber(L, 2);
	}

	if (async_add_fd_read(wsgi_req, fd, timeout)) {
		lua_pushstring(L, "unable to call async_add_fd_read()");
        	lua_error(L);
        	return 0;
        }
end:
        lua_pushnil(L);
        return 1;
}

static int uwsgi_api_wait_fd_write(lua_State *L) {
        uint8_t argc = lua_gettop(L);
        if (argc == 0) goto end;

        struct wsgi_request *wsgi_req = current_wsgi_req();

        int fd = lua_tonumber(L, 1);
        int timeout = 0;
        if (argc > 1) {
                timeout = lua_tonumber(L, 2);
        }

        if (async_add_fd_write(wsgi_req, fd, timeout)) {
                lua_pushstring(L, "unable to call async_add_fd_write()");
                lua_error(L);
                return 0;
        }
end:
        lua_pushnil(L);
        return 1;
}

static int uwsgi_api_async_connect(lua_State *L) {
        uint8_t argc = lua_gettop(L);
        if (argc == 0) goto end;

	int fd = uwsgi_connect((char *)lua_tostring(L, 1), 0, 1);
	lua_pushnumber(L, fd);
	return 1;
end:
        lua_pushnil(L);
        return 1;
}

static int uwsgi_api_is_connected(lua_State *L) {
        uint8_t argc = lua_gettop(L);
        if (argc == 0) goto end;
	int fd = lua_tonumber(L, 1);
	if (uwsgi_is_connected(fd)) {
		lua_pushboolean(L, 1);
		return 1;
	}
	lua_pushboolean(L, 0);
        return 1;
end:
        lua_pushnil(L);
        return 1;
}

static int uwsgi_api_close(lua_State *L) {
        uint8_t argc = lua_gettop(L);
        if (argc == 0) goto end;
        int fd = lua_tonumber(L, 1);
	close(fd);
end:
        lua_pushnil(L);
        return 1;
}


static int uwsgi_api_ready_fd(lua_State *L) {
	struct wsgi_request *wsgi_req = current_wsgi_req();
        int fd = uwsgi_ready_fd(wsgi_req);
        lua_pushnumber(L, fd);
        return 1;
}

static int uwsgi_api_websocket_handshake(lua_State *L) {
	uint8_t argc = lua_gettop(L);

	const char *key = NULL, *origin = NULL, *proto = NULL;
	size_t key_len = 0, origin_len = 0, proto_len = 0;
		
	if (argc > 0) {
		key = lua_tolstring(L, 1, &key_len);
		if (argc > 1) {
			origin = lua_tolstring(L, 2, &origin_len);
			if (argc > 2) {
				proto = lua_tolstring(L, 3, &proto_len);
			}
		}
	}

	struct wsgi_request *wsgi_req = current_wsgi_req();
	if (uwsgi_websocket_handshake(wsgi_req, (char *)key, key_len, (char *)origin, origin_len, (char *) proto, proto_len)) {
		goto error;
	}	

	lua_pushnil(L);
        return 1;

error:
	lua_pushstring(L, "unable to complete websocket handshake");
	lua_error(L);
	return 0;	
}

static int uwsgi_api_websocket_send(lua_State *L) {
	uint8_t argc = lua_gettop(L);
        if (argc == 0) goto error;

	size_t message_len = 0;
	const char *message = lua_tolstring(L, 1, &message_len);
	struct wsgi_request *wsgi_req = current_wsgi_req();

        if (uwsgi_websocket_send(wsgi_req, (char *) message, message_len)) {
		goto error;
        }
	lua_pushnil(L);
        return 1;
error:
        lua_pushstring(L, "unable to send websocket message");    
        lua_error(L);
        return 0;
}

static int uwsgi_api_websocket_send_binary(lua_State *L) {
        uint8_t argc = lua_gettop(L);
        if (argc == 0) goto error;

        size_t message_len = 0;
        const char *message = lua_tolstring(L, 1, &message_len);
        struct wsgi_request *wsgi_req = current_wsgi_req();

        if (uwsgi_websocket_send_binary(wsgi_req, (char *) message, message_len)) {
                goto error;
        }
	lua_pushnil(L);
        return 1;
error:
        lua_pushstring(L, "unable to send websocket binary message");      
        lua_error(L);
        return 0;
}

static int uwsgi_api_websocket_send_from_sharedarea(lua_State *L) {
        uint8_t argc = lua_gettop(L);
        if (argc < 2) goto error;

	int id = lua_tonumber(L, 1);
	uint64_t pos = lua_tonumber(L, 2);
	uint64_t len = 0;
	if (argc > 2) {
		len = lua_tonumber(L, 3);
	}
        struct wsgi_request *wsgi_req = current_wsgi_req();

	if (uwsgi_websocket_send_from_sharedarea(wsgi_req, id, pos, len)) {
                goto error;
        }
	lua_pushnil(L);
        return 1;
error:
        lua_pushstring(L, "unable to send websocket message from sharedarea");      
        lua_error(L);
        return 0;
}

static int uwsgi_api_websocket_send_binary_from_sharedarea(lua_State *L) {
        uint8_t argc = lua_gettop(L);
        if (argc < 2) goto error;

        int id = lua_tonumber(L, 1);
        uint64_t pos = lua_tonumber(L, 2);
        uint64_t len = 0;
        if (argc > 2) {
                len = lua_tonumber(L, 3);
        }
        struct wsgi_request *wsgi_req = current_wsgi_req();

        if (uwsgi_websocket_send_binary_from_sharedarea(wsgi_req, id, pos, len)) {
                goto error;
        }
        lua_pushnil(L);
        return 1;
error:
        lua_pushstring(L, "unable to send websocket message from sharedarea");
        lua_error(L);
        return 0;
}

static int uwsgi_api_websocket_recv(lua_State *L) {
	struct wsgi_request *wsgi_req = current_wsgi_req();
        struct uwsgi_buffer *ub = uwsgi_websocket_recv(wsgi_req);
	if (!ub) {
        	lua_pushstring(L, "unable to receive websocket message");
        	lua_error(L);
        	return 0;
	}
	lua_pushlstring(L, ub->buf, ub->pos);
	uwsgi_buffer_destroy(ub);
	return 1;
}

static int uwsgi_api_websocket_recv_nb(lua_State *L) {
        struct wsgi_request *wsgi_req = current_wsgi_req();
        struct uwsgi_buffer *ub = uwsgi_websocket_recv_nb(wsgi_req);
        if (!ub) {
                lua_pushstring(L, "unable to receive websocket message");
                lua_error(L);
                return 0;
        }
        lua_pushlstring(L, ub->buf, ub->pos);
        uwsgi_buffer_destroy(ub);
        return 1;
}

static int uwsgi_api_cache_get(lua_State *L) {

        char *value ;
        uint64_t valsize;
	size_t keylen;
	const char *key ;
	const char *cache = NULL;
	uint8_t argc = lua_gettop(L);

	if (argc == 0) goto error;

	if (lua_isstring(L, 1)) {
		// get the key
		key = lua_tolstring(L, 1, &keylen);
		if (argc > 1) {
			cache = lua_tostring(L, 2);
		}
        	value = uwsgi_cache_magic_get((char *)key, keylen, &valsize, NULL, (char *)cache);
        	if (value) {
                	lua_pushlstring(L, value, valsize);
			free(value);
			return 1;
        	}
	}

error:
	lua_pushnil(L);
        return 1;

}

static int uwsgi_api_req_fd(lua_State *L) {

	struct wsgi_request *wsgi_req = current_wsgi_req();
	
	lua_pushnumber(L, wsgi_req->fd);
	return 1;
}

static int uwsgi_api_lock(lua_State *L) {

	int lock_num = 0;

	// the spooler cannot lock resources
	if (uwsgi.i_am_a_spooler) {
		lua_pushstring(L, "The spooler cannot lock/unlock resources");
		lua_error(L);
	}

	if (lua_gettop(L) > 0) {
		lock_num = lua_isnumber(L, 1) ? lua_tonumber(L, 1) : -1;
		if (lock_num < 0 || lock_num > uwsgi.locks) {
			lua_pushstring(L, "Invalid lock number");
	    		lua_error(L);
	  	}
	}
	
	uwsgi_lock(uwsgi.user_lock[lock_num]);

	return 0;
}


static int uwsgi_api_unlock(lua_State *L) {

	int lock_num = 0;

	// the spooler cannot lock resources
	if (uwsgi.i_am_a_spooler) {
		lua_pushstring(L, "The spooler cannot lock/unlock resources");
		lua_error(L);
	}

	if (lua_gettop(L) > 0) {
		lock_num = lua_isnumber(L, 1) ? lua_tonumber(L, 1) : -1;
		if (lock_num < 0 || lock_num > uwsgi.locks) {
			lua_pushstring(L, "Invalid lock number");
	    		lua_error(L);
	  	}
	}
	
	uwsgi_unlock(uwsgi.user_lock[lock_num]);

	return 0;
}

static int uwsgi_lua_input(lua_State *L) {

	struct wsgi_request *wsgi_req = current_wsgi_req();
	ssize_t sum = 0;

	int n = lua_gettop(L);

	if (n > 1) {
		sum = lua_tonumber(L, 2);
	}

	ssize_t rlen = 0;

        char *buf = uwsgi_request_body_read(wsgi_req, sum, &rlen);
        if (buf) {
		lua_pushlstring(L, buf, rlen);
                return 1;
        }

	return 0;
}

static int uwsgi_api_async_id_get(lua_State *L) {

	struct wsgi_request *wsgi_req = current_wsgi_req();
	
	lua_pushnumber(L, wsgi_req->async_id);
	
	return 1;
}

static const luaL_Reg uwsgi_api[] = {
  {"log", uwsgi_api_log},
  {"connection_fd", uwsgi_api_req_fd},

  {"cache_get", uwsgi_api_cache_get},
  {"cache_set", uwsgi_api_cache_set},
  {"cache_update", uwsgi_api_cache_update},
  {"cache_del", uwsgi_api_cache_del},
  {"cache_exists", uwsgi_api_cache_exists},
  {"cache_clear", uwsgi_api_cache_clear},

  {"register_signal", uwsgi_api_register_signal},
  {"register_rpc", uwsgi_api_register_rpc},
  {"rpc", uwsgi_api_rpc},
  {"req_input_read", uwsgi_lua_input},

  {"websocket_handshake", uwsgi_api_websocket_handshake},
  {"websocket_recv", uwsgi_api_websocket_recv},
  {"websocket_recv_nb", uwsgi_api_websocket_recv_nb},
  {"websocket_send", uwsgi_api_websocket_send},
  {"websocket_send_from_sharedarea", uwsgi_api_websocket_send_from_sharedarea},
  {"websocket_send_binary", uwsgi_api_websocket_send_binary},
  {"websocket_send_binary_from_sharedarea", uwsgi_api_websocket_send_binary_from_sharedarea},

  {"lock", uwsgi_api_lock},
  {"unlock", uwsgi_api_unlock},

  {"async_sleep", uwsgi_api_async_sleep},
  {"async_connect", uwsgi_api_async_connect},
  {"async_id_get", uwsgi_api_async_id_get},
  {"is_connected", uwsgi_api_is_connected},
  {"close", uwsgi_api_close},
  {"wait_fd_read", uwsgi_api_wait_fd_read},
  {"wait_fd_write", uwsgi_api_wait_fd_write},
  {"ready_fd", uwsgi_api_ready_fd},

  {NULL, NULL}
};


static int uwsgi_lua_init(){
	int i;

	uwsgi_log("Initializing Lua environment... ");
	
	ulua.state = uwsgi_malloc(sizeof(lua_State**) * uwsgi.numproc);
	
	for (i=0;i<uwsgi.numproc;i++) {
		ulua.state[i] = uwsgi_malloc(sizeof(lua_State*) * uwsgi.cores);
	}
	
	uwsgi_log("%d lua_States (with %d lua_Threads)\n", uwsgi.numproc, uwsgi.cores);
	
	if(ulua.gc_full == 0) {
		ulua.gc_full = LUA_GCSTEP;
	} else {
		ulua.gc_full = LUA_GCCOLLECT;
	}
	
	// ok the lua engine is ready
	return 0;
}


static void uwsgi_lua_init_state(lua_State **Ls, int wid, int sid, int cores) {
	int i;
	int uslnargs;
	lua_State *L;

	// spawn worker state		
	Ls[0] = luaL_newstate();
	L = Ls[0];

	// init worker state
	luaL_openlibs(L);
	luaL_newuwsgilib(L, uwsgi_api);

	lua_pushstring(L, UWSGI_VERSION);
	lua_setfield(L, -2, "version");

	lua_pushnumber(L, wid);
	lua_setfield(L, -2, "mywid");
	
	lua_pushnumber(L, sid);
	lua_setfield(L, -2, "mysid");
	
	// reserve ref 1 for ws func
	lua_pushvalue(L, -1);
	luaL_ref(L, LUA_REGISTRYINDEX);

	// init main app
	uslnargs = lua_gettop(L);

	struct uwsgi_string_list *usl = ulua.load;
	while(usl) {
		if (luaL_dofile(L, usl->value)) {
			uwsgi_log("unable to load Lua file %s: %s\n", usl->value, lua_tostring(L, -1));
			lua_pop(L, 1);
		}
		usl = usl->next;
	}
			
	uslnargs = lua_gettop(L) - uslnargs;
			
	if (ulua.filename) {
		if (luaL_loadfile(L, ulua.filename)) {
			uwsgi_log("unable to load Lua file %s: %s\n", ulua.filename, lua_tostring(L, -1));
			lua_pop(L, 1);
		} else {
			if (lua_pcall(L, uslnargs, 1, 0) != 0) {
				uwsgi_log("%s\n", lua_tostring(L, -1));
				lua_pop(L, 1);
				uslnargs = 0;
			} else {
				uslnargs = 1;
			}
		}
	}
			
	//we need only one, the last one
	if (uslnargs > 1) {
		lua_replace(L, -uslnargs);
				
		if (uslnargs > 2) {
			lua_pop(L, uslnargs - 2);
		}
	}
			
	// table ??
	if (lua_istable(L, -1)) {
		lua_pushstring(L, "run");
		lua_gettable(L, -1);
		lua_replace(L, -1);
	}
			
	// no app ???
	if (uslnargs == 0 || !lua_isfunction(L, -1)) {
		// loading dummy
		if (uslnargs > 0) {
			lua_pop(L, uslnargs);
		}		
			
		uwsgi_log("Can't find WSAPI entry point (no function, nor a table with function'run').\n");
		i = luaL_dostring(L, "return function() return '500'; end");
	}
	
	lua_rawseti(L, LUA_REGISTRYINDEX, 1);
			
	//init additional threads for current worker
	if(cores > 0) {
			
		lua_newtable(L);
				
		for(i=1;i<cores;i++) {

			// create thread and save it
			Ls[i] = lua_newthread(L);
			lua_rawseti(L, -2, i);

			// push app from master and move to new
			//lua_pushvalue(L, -2);
			//lua_xmove(L, Ls[i], 1);
		}
		
		lua_setfield(L, -2, "luathreads");
	}
			
	// and the worker is ready!
	//lua_remove(L, -2);
	lua_pop(L, 1);
	
	lua_gc(L, LUA_GCCOLLECT, 0);
			
	uwsgi_log("inited lua_State %d for worker %d\n", sid, wid);
}

static int uwsgi_lua_request(struct wsgi_request *wsgi_req) {

	int i;
	const char *http, *http2;
	size_t slen, slen2;
	lua_State *L = ULUA_WORKER_STATE[wsgi_req->async_id];

	if (wsgi_req->async_status == UWSGI_AGAIN) {
		if (lua_type(L, -1) != LUA_TFUNCTION) {
			// wrong async_status status!
			return -1;
		}
async_coroutine:
		while (lua_pcall(L, 0, 1, 0) == 0) {
			if (lua_isstring(L, -1)) {
				http = lua_tolstring(L, -1, &slen);
				uwsgi_response_write_body_do(wsgi_req, (char *)http, slen);
			} else if (lua_isnil(L, -1)) { // posible dead coroutine
				lua_pop(L, 1);
				lua_pushvalue(L, -1);
				continue;	// retry
			}
			
			lua_pop(L, 1);
			lua_pushvalue(L, -1);
			return UWSGI_AGAIN;
		}
		goto clear;
	}

	/* Standard WSAPI request */
	if (!wsgi_req->len) {
		uwsgi_log( "Empty lua request. skip.\n");
		return -1;
	}

	if (uwsgi_parse_vars(wsgi_req)) {
		return -1;
	}

	// put function in the stack
	//lua_getfield(L, LUA_GLOBALSINDEX, "run");
	lua_rawgeti(L, LUA_REGISTRYINDEX, 1);
	//lua_pushvalue(L, -1);

	// put cgi vars in the stack

	lua_newtable(L);
	lua_pushstring(L, "");
	lua_setfield(L, -2, "CONTENT_TYPE");
	
	for(i=0;i<wsgi_req->var_cnt;i+=2) {
		lua_pushlstring(L, wsgi_req->hvec[i].iov_base, wsgi_req->hvec[i].iov_len);
		lua_pushlstring(L, wsgi_req->hvec[i+1].iov_base, wsgi_req->hvec[i+1].iov_len);
		lua_rawset(L, -3);
	}


	// put "input" table
	lua_newtable(L);
	lua_pushcfunction(L, uwsgi_lua_input);
	lua_setfield(L, -2, "read");
	lua_setfield(L, -2, "input");
	

#ifdef UWSGI_DEBUG
	uwsgi_log("stack pos %d\n", lua_gettop(L));
#endif

	// call function

	if (lua_pcall(L, 1, 3, 0) != 0) {
		uwsgi_log("%s\n", lua_tostring(L, -1));
		lua_pop(L, 1);
		lua_pushvalue(L, -1);
		goto clear2;
	}

	//uwsgi_log("%d %s %s %s\n",i,lua_typename(L, lua_type(L, -3)), lua_typename(L, lua_type(L, -2)) ,  lua_typename(L, lua_type(L, -1)));

	// send status
	if (lua_isstring(L, -3)) {
		http = lua_tolstring(L, -3, &slen);
		if (uwsgi_response_prepare_headers(wsgi_req, (char *) http, slen))
			goto clear2;
	}
	else {
		uwsgi_log("[uwsgi-lua] invalid response status !!!\n");
		// let's continue 
	}

	// send headers
	if (lua_type(L, -2) == LUA_TTABLE) {
		lua_pushnil(L);
		while(lua_next(L, -3) != 0) {
			http = lua_tolstring(L, -2, &slen);

			if (lua_type(L, -1) == LUA_TTABLE) {
				for (i = 1; /*empty*/ ; ++i) {
					lua_rawgeti(L, -1, i);

					if (lua_isnil(L, -1)) {
						lua_pop(L, 1);
						break;
					}

					http2 = lua_tolstring(L, -1, &slen2);
					uwsgi_response_add_header(wsgi_req, (char *) http, slen, (char *) http2, slen2);
					lua_pop(L, 1);
				}
			}
			else {
				http2 = lua_tolstring(L, -1, &slen2);
				uwsgi_response_add_header(wsgi_req, (char *) http, slen, (char *) http2, slen2);
			}
			lua_pop(L, 1);
		}
	}

	// send body with coroutine or copy from string
	lua_pushvalue(L, -1);

	if (lua_type(L, -1) == LUA_TFUNCTION) {
		if (uwsgi.async > 0) {	
			goto async_coroutine;
		}
	
		while ( lua_pcall(L, 0, 1, 0) == 0) {
			if (lua_isstring(L, -1)) {
				http = lua_tolstring(L, -1, &slen);
				uwsgi_response_write_body_do(wsgi_req, (char *)http, slen);
			}
			
			lua_pop(L, 1);
			lua_pushvalue(L, -1);
		}
	}
	else if (lua_isstring(L, -1)) {
		http = lua_tolstring(L, -1, &slen);
		uwsgi_response_write_body_do(wsgi_req, (char *) http, slen);
	}

clear:
	lua_pop(L, 4);
clear2:
	// set frequency
	if (!ulua.gc_freq || 
		(uwsgi.threads == 1 && uwsgi.workers[uwsgi.mywid].requests % ulua.gc_freq == 0) || 
		(uwsgi.threads > 1 && uwsgi.workers[uwsgi.mywid].cores[wsgi_req->async_id].requests % ulua.gc_freq == 0)) {
			lua_gc(L, ulua.gc_full, 0);
	}

	return UWSGI_OK;

}

static void uwsgi_lua_after_request(struct wsgi_request *wsgi_req) {

	log_request(wsgi_req);
}


static int uwsgi_lua_magic(char *mountpoint, char *lazy) {

	if (!strcmp(lazy+strlen(lazy)-4, ".lua")) {
                ulua.filename = lazy;
                return 1;
        }
        else if (!strcmp(lazy+strlen(lazy)-3, ".ws")) {
                ulua.filename = lazy;
                return 1;
        }


	return 0;
}

static char *uwsgi_lua_code_string(char *id, char *code, char *func, char *key, uint16_t keylen) {

	static struct lua_State *L = NULL;

	if (!L) {
		L = luaL_newstate();
                luaL_openlibs(L);
                if (luaL_loadfile(L, code) || lua_pcall(L, 0, 0, 0)) {
                	uwsgi_log("unable to load file %s: %s\n", code, lua_tostring(L, -1));
			lua_close(L);
			L = NULL;
			return NULL;
                }
		lua_getglobal(L, func);
		if (!lua_isfunction(L,-1)) {
			uwsgi_log("unable to find %s function in lua file %s\n", func, code);
			lua_close(L);
			L = NULL;
			return NULL;
		}
		lua_pushnil(L);
	}

	
	lua_pop(L, 1);

	lua_pushvalue(L, -1);
	lua_pushlstring(L, key, keylen);

#ifdef UWSGI_DEBUG
	uwsgi_log("stack pos %d %.*s\n", lua_gettop(L), keylen, key);
#endif

        if (lua_pcall(L, 1, 1, 0) != 0) {
                uwsgi_log("error running function `f': %s", lua_tostring(L, -1));
                return NULL;

        }

	if (lua_isstring(L, -1)) {
                const char *ret = lua_tostring(L, -1);
		return (char *)ret;
        }

        return NULL;
}

static int uwsgi_lua_signal_handler(uint8_t sig, void *handler) {

	struct wsgi_request *wsgi_req = current_wsgi_req();
	
	lua_State *L = ULUA_WORKER_STATE[wsgi_req->async_id];

#ifdef UWSGI_DEBUG
	uwsgi_log("managing signal handler on core %d\n", wsgi_req->async_id);
#endif

	lua_rawgeti(L, LUA_REGISTRYINDEX, (long) handler);

	lua_pushnumber(L, sig);

	if (lua_pcall(L, 1, 1, 0) != 0) {
		uwsgi_log("error running function `f': %s",
                 lua_tostring(L, -1));

		return -1;
	}

	return 0;
	
}

static uint64_t uwsgi_lua_rpc(void * func, uint8_t argc, char **argv, uint16_t argvs[], char **buffer) {

        uint8_t i;
        const char *sv;
        size_t sl;
	long lfunc = (long) func;
	int ifunc = lfunc;

	struct wsgi_request *wsgi_req = current_wsgi_req();
	
	lua_State *L = ULUA_WORKER_STATE[wsgi_req->async_id];

#ifdef UWSGI_DEBUG
	uwsgi_log("get function %d\n", ifunc);
#endif
	lua_rawgeti(L, LUA_REGISTRYINDEX, ifunc);

        for(i=0;i<argc;i++) {
		lua_pushlstring(L, argv[i], argvs[i]);
        }

        if (lua_pcall(L, argc, 1, 0) != 0) {
		uwsgi_log("error running function `f': %s", lua_tostring(L, -1));
		return 0;
        }

	
	sv = lua_tolstring(L, -1, &sl);

#ifdef UWSGI_DEBUG
	uwsgi_log("sv = %s sl = %lu\n", sv, (unsigned long) sl);
#endif
	if (sl > 0) {
		*buffer = uwsgi_malloc(sl);
		memcpy(*buffer, sv, sl);
		lua_pop(L, 1);
		return sl;
	}

	lua_pop(L, 1);
        return 0;

}

static void uwsgi_lua_configurator_array(lua_State *L) { 

	int i;
	int n = lua_rawlen(L, -3);

	for(i=1;i<=n;i++) {
		lua_rawgeti(L, 1, i);
		if (lua_istable(L, -1)) {
                	lua_pushnil(L);
                        while (lua_next(L, -2) != 0) {
                        	char *key = uwsgi_str((char *)lua_tostring(L, -2));
                                char *value = uwsgi_str((char *)lua_tostring(L, -1));
                                add_exported_option(key, value, 0);
                                lua_pop(L, 1);
                        }
                }
	}
}


static void uwsgi_lua_configurator(char *filename, char *magic_table[]) {
	size_t len = 0;
	uwsgi_log_initial("[uWSGI] getting Lua configuration from %s\n", filename);
	char *code = uwsgi_open_and_read(filename, &len, 1, magic_table);
	lua_State *L = luaL_newstate();
	if (!L) {
		uwsgi_log("unable to initialize Lua state for configuration\n");
		exit(1);
	}
        luaL_openlibs(L);
	if (luaL_dostring(L, code) != 0) {
		uwsgi_log("error running Lua configurator: %s\n", lua_tostring(L, -1));
		exit(1);
	}
	free(code);

	if (!lua_istable(L, -1)) {
		uwsgi_log("Lua configurator has to return a table !!!\n");
		exit(1);
	}

	lua_pushnil(L);
	// we always use uwsgi_str to avoid GC destroying our strings
	// and to be able to call lua_close at the end
	while (lua_next(L, -2) != 0) {
		// array ?
		if (lua_isnumber(L, -2)) {
			uwsgi_lua_configurator_array(L);
			break;
		}
		// dictionary
		else {
			char *key = uwsgi_str((char *)lua_tostring(L, -2));
			if (lua_istable(L, -1)) {
				lua_pushnil(L);
				while (lua_next(L, -2) != 0) {
					char *value = uwsgi_str((char *)lua_tostring(L, -1));
					add_exported_option(key, value, 0);
					lua_pop(L, 1);
				}	
			}
			else {
				char *value = uwsgi_str((char *)lua_tostring(L, -1));
				add_exported_option(key, value, 0);
			}
		}
		lua_pop(L, 1);
	}

	// this will destroy the whole Lua state
	lua_close(L);
}

static void uwsgi_register_lua_features() {
	uwsgi_register_configurator(".lua", uwsgi_lua_configurator);
}

static void uwsgi_lua_hijack(void) {
        if (ulua.shell && uwsgi.mywid == 1) {
                uwsgi.workers[uwsgi.mywid].hijacked = 1;
                uwsgi.workers[uwsgi.mywid].hijacked_count++;
                // re-map stdin to stdout and stderr if we are logging to a file
                if (uwsgi.logfile) {
                        if (dup2(0, 1) < 0) {
                                uwsgi_error("dup2()");
                        }
                        if (dup2(0, 2) < 0) {
                                uwsgi_error("dup2()");
                        }
                }
                int ret = -1;
		// run in the first state
		lua_State *L = ULUA_WORKER_STATE[0];		
		lua_getglobal(L, "debug");
		lua_getfield(L, -1, "debug");
		ret = lua_pcall(L, 0, 0, 0);
                if (ret == 0) {
                        exit(UWSGI_QUIET_CODE);
                }
                exit(0);
        }

}

static void uwsgi_lua_init_apps() {
	int i,j,sid;
	
	//cores per lua thread
	int cores = uwsgi.threads > 1 ? 1 : uwsgi.cores;

	if (uwsgi.mywid > 0) {	// lazy app
		sid = ULUA_MYWID*uwsgi.threads;
	
		for(i=0;i<uwsgi.threads;i++) {
			uwsgi_lua_init_state(&(ULUA_WORKER_STATE[i]), uwsgi.mywid, sid + i, cores);
		}
		
	} else {
		for(j=0;j<uwsgi.numproc;j++){
			sid = j*uwsgi.threads;
			
			for(i=0;i<uwsgi.threads;i++) {
				uwsgi_lua_init_state(&(ulua.state[j][i]), j + 1, sid + i, cores);
			}
			
		}
	}
}
	
struct uwsgi_plugin lua_plugin = {

	.name = "lua",
	.modifier1 = 6,
	.init = uwsgi_lua_init,
	.options = uwsgi_lua_options,
	.request = uwsgi_lua_request,
	.after_request = uwsgi_lua_after_request,
	
	.init_apps = uwsgi_lua_init_apps,
	
	.magic = uwsgi_lua_magic,
	.signal_handler = uwsgi_lua_signal_handler,

	.hijack_worker = uwsgi_lua_hijack,

	.code_string = uwsgi_lua_code_string,
	.rpc = uwsgi_lua_rpc,

	.on_load = uwsgi_register_lua_features,
};

