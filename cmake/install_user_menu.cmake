# Idempotently register the CopyToPoints plugin folder in the user's
# ~/.nuke/menu.py.  CMAKE_INSTALL_PREFIX is expected to be ~/.nuke.
#
# The registration lives in ~/.nuke/init.py so it works in terminal (-t)
# sessions too; menu.py inside the plugin folder adds the toolbar entry.

set(_marker_begin "# --- CopyToPoints (auto-added by cmake --install) ---")
set(_marker_end   "# --- end CopyToPoints ---")
set(_block "${_marker_begin}\nimport nuke\nnuke.pluginAddPath('./CopyToPoints')\n${_marker_end}\n")

foreach(_target IN ITEMS init.py)
    set(_path "${CMAKE_INSTALL_PREFIX}/${_target}")
    set(_existing "")
    if(EXISTS "${_path}")
        file(READ "${_path}" _existing)
    endif()
    string(FIND "${_existing}" "${_marker_begin}" _pos)
    if(_pos EQUAL -1)
        if(NOT _existing STREQUAL "" AND NOT _existing MATCHES "\n$")
            set(_existing "${_existing}\n")
        endif()
        file(WRITE "${_path}" "${_existing}${_block}")
        message(STATUS "CopyToPoints: registered plugin path in ${_path}")
    else()
        message(STATUS "CopyToPoints: ${_path} already registers the plugin path (skipped)")
    endif()
endforeach()
