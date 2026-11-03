# Keep editable Lazarus forms in source control and embed them in the plugin.
function(s3cmd_embed_dialog name output)
    set(path "${PROJECT_SOURCE_DIR}/src/doublecmd/${name}.lfm")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(READ "${path}" form)
    set(content "#pragma once\n\nnamespace s3cmd::embedded_dialogs {\ninline constexpr char ${name}[] = R\"S3CMD_LFM(${form})S3CMD_LFM\";\n}\n")
    file(CONFIGURE OUTPUT "${output}" CONTENT "${content}" @ONLY)
endfunction()
