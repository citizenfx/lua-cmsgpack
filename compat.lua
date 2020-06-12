--[[
    Compatibility layer for:
        lua-MessagePack : <http://fperrad.github.io/lua-MessagePack/>
--]]

local m = require('cmsgpack')
local msgpack_pack = m.pack
local msgpack_unpack = m.unpack

local function argerror (caller, narg, extramsg)
    error("bad argument #" .. tostring(narg) .. " to " .. caller .. " (" .. extramsg .. ")")
end

local function typeerror (caller, narg, arg, tname)
    argerror(caller, narg, tname .. " expected, got " .. type(arg))
end

local function checktype (caller, narg, arg, tname)
    if type(arg) ~= tname then
        typeerror(caller, narg, arg, tname)
    end
end

---------------------------------------
---------- Cursors/Iterators ----------
---------------------------------------

--[[ Requires iterator functions to be re-integrated --]]

---------------------------------------
--------------- Options ---------------
---------------------------------------

function m.set_number(number)
    if number == 'float' then
        m.setoption("float", true)
    elseif number == 'double' then
        m.setoption("double", true)
    else
        argerror('set_number', 1, "invalid option '" .. number .."'")
    end
end

function m.set_array(array)
    if array == "without_hole" or array == "with_hole" or array == "always_as_map" then
        m.setoption(array, true)
    else
        argerror('set_array', 1, "invalid option '" .. array .."'")
    end
end

m.small_lua = m.getoption("small_lua")
m.full64bits = m.getoption("full64bits")
m.long_double = m.getoption("long_double")

_G.msgpack = m