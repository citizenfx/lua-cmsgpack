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

--[[
Example:
    Vector3 = {
        -- Metamethods
        -- ...

        -- Serialization
        __ext = 0x4,
        __pack = function(self, type)
            return m.pack(self.x, self.y, self.z)
        end,
        __unpack = function(encoded, t)
            local x,y,z = m.unpack(encoded)
            return setmetatable({ x = x, y = y, z = z }, Vector3)
        end,
    }
--]]

--[[ msgpack timestamp extension that packs its own data --]]
function m.set_timestamp()
    m.extend({
        __ext = -1,
        __pack = function(time, t)
            local buffer = { }
            if (time.tv_sec >> 34) == 0 then
                local data64 = (time.tv_nsec << 34) | time.tv_sec;
                if (data64 & 0xffffffff00000000) == 0 then
                    local data32 = data64 & 0x00000000FFFFFFFF;
                    buffer[#buffer + 1] = string.char(0xD6)
                    buffer[#buffer + 1] = string.pack('>i1', -1)
                    buffer[#buffer + 1] = string.pack('>I4', data32)
                else
                    buffer[#buffer + 1] = string.char(0xD7)
                    buffer[#buffer + 1] = string.pack('>i1', -1)
                    buffer[#buffer + 1] = string.pack('>I8', data64)
                end
            else
                buffer[#buffer + 1] = string.char(0xC7, 12)
                buffer[#buffer + 1] = string.pack('>i1', -1)
                buffer[#buffer + 1] = string.pack('>I4', time.tv_nsec)
                buffer[#buffer + 1] = string.pack('>I8', time.tv_sec)
            end
            -- true: tells the encoder the extension body is already packed
            return table.concat(buffer),true
        end,

        __unpack = function(s, t)
            local tv_sec, tv_nsec = 0, 0
            if s:len() == 4 then
                tv_sec = string.unpack('>I4', s)
            elseif s:len() == 8 then
                local data64 = string.unpack('>I8', s)
                tv_nsec = data64 >> 34
                tv_sec = data64 & 0x00000003ffffffff
            elseif s:len() == 12 then
                local data32,data64 = string.unpack('>I4>I8', s)
                tv_nsec = data32;
                tv_sec = data64;
            else
                error(("Invalid timestamp: %d"):format(s:len()))
            end
            return { tv_sec = tv_sec, tv_nsec = tv_nsec }
        end,
    })
end

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