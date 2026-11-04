include ../mingw-shell.mk

WINCC ?= i686-w64-mingw32-gcc
WINDRES ?= i686-w64-mingw32-windres
ifeq ($(OS),Windows_NT)
ifeq ($(origin CC),default)
CC := $(WINCC)
endif
endif
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c99
# qxpdisp reuses qxpmp/qxp_texture_op.h; the driver build gets the same
# root through INCLUDES=.. in qxpdisp/sources.
CFLAGS += -I.
BUILD := build

.PHONY: all test preinstall-test d3d9-shader-objects-test \
	d3d9-draw-state-test d3d9-shader-link-test \
	d3d9-draw-prepare-test d3d9-draw-execute-test \
	d3d9-draw-backing-test d3d9-draw-envelope-test \
	d3d9-draw-access-test d3d9-draw-capture-test \
	d3d9-draw-lifetime-test d3d9-draw-source-test d3d-texblt-test \
	ps20-glsl-smoke vs20-glsl-smoke \
	sm20-pair-smoke check-arbvp glsl-compat-smoke \
	arbvp-compat-smoke compact-arbvp-compat-smoke \
	clip-glsl-compat-smoke surface-copy-smoke surface-readback-smoke clean

all: $(BUILD)/qxpescape_test $(BUILD)/qxp_escape_win32.o \
	$(BUILD)/qxp_shader_decode_test.exe \
	$(BUILD)/qxp_shader_test.exe $(BUILD)/qxp_vshader_test.exe \
	$(BUILD)/qxp_vdecl_test.exe \
	$(BUILD)/qxp_vdecl9_test.exe \
	$(BUILD)/qxp_d3d9_shader_objects_test.exe \
	$(BUILD)/qxp_d3d9_draw_state_test.exe \
	$(BUILD)/qxp_d3d9_shader_link_test.exe \
	$(BUILD)/qxp_d3d9_draw_prepare_test.exe \
	$(BUILD)/qxp_d3d9_draw_execute_test.exe \
	$(BUILD)/qxp_d3d9_draw_backing_test.exe \
	$(BUILD)/qxp_d3d9_draw_envelope_test.exe \
	$(BUILD)/qxp_d3d9_draw_access_test.exe \
	$(BUILD)/qxp_d3d9_draw_capture_test.exe \
	$(BUILD)/qxp_d3d9_draw_lifetime_test.exe \
	$(BUILD)/qxp_d3d9_draw_source_test.exe \
	$(BUILD)/qxp_d3d_texblt_test.exe \
	$(BUILD)/qxp_pshader_glsl_test.exe \
	$(BUILD)/qxp_ffpshader_glsl_test.exe \
	$(BUILD)/qxp_positiont_vshader_glsl_test.exe \
	$(BUILD)/qxp_vshader20_glsl_test.exe \
	$(BUILD)/qxp_hwtl_test.exe \
	$(BUILD)/qxp_vertex_test.exe \
	$(BUILD)/qxp_profile_timeline_test.exe \
	$(BUILD)/qxp_d3d9_sampler_test.exe \
	$(BUILD)/qxp_packet_test.exe $(BUILD)/qxp_float_test.exe \
	$(BUILD)/qxp_program_test.exe \
	$(BUILD)/qxp_texture_op_test.exe \
	$(BUILD)/qxp_context_lifetime_test.exe \
	$(BUILD)/qxp_shader_owner_test.exe \
	$(BUILD)/qxpcoinst_test.exe \
	$(BUILD)/qxpcoin2.dll

include ../../tools/version.mk

$(BUILD):
	mkdir $@

$(BUILD)/qxpescape_test: qxpescape_test.c qxp_escape.c qxp_escape.h \
		qxpdisp_escape.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ qxpescape_test.c qxp_escape.c

$(BUILD)/qxp_escape_win32.o: qxp_escape.c qxp_escape.h | $(BUILD)
	$(WINCC) $(CFLAGS) -march=i686 -c -o $@ qxp_escape.c

$(BUILD)/qxp_shader_decode_test.exe: qxp_shader_decode_test.c \
		qxpdisp/qxp_shader_decode.c qxpdisp/qxp_shader_ir.h | $(BUILD)
	$(WINCC) $(CFLAGS) -march=i686 -Iqxpdisp -o $@ \
		qxp_shader_decode_test.c qxpdisp/qxp_shader_decode.c

$(BUILD)/qxp_shader_test.exe: qxp_shader_test.c \
		qxpdisp/qxp_shader.c qxpdisp/qxp_shader.h | $(BUILD)
	$(WINCC) $(CFLAGS) -march=i686 -Iqxpdisp -o $@ \
		qxp_shader_test.c qxpdisp/qxp_shader.c

$(BUILD)/qxp_vshader_test.exe: qxp_vshader_test.c \
		qxpdisp/qxp_vshader.c qxpdisp/qxp_vshader.h \
		qxpdisp/qxp_vshader_glsl.c qxpdisp/qxp_vshader_glsl.h | $(BUILD)
	$(WINCC) $(CFLAGS) -march=i686 -Iqxpdisp -o $@ \
		qxp_vshader_test.c qxpdisp/qxp_vshader.c \
		qxpdisp/qxp_vshader_glsl.c

$(BUILD)/qxp_vdecl_test.exe: qxp_vdecl_test.c \
		qxpdisp/qxp_vdecl.c qxpdisp/qxp_vdecl.h | $(BUILD)
	$(WINCC) $(CFLAGS) -march=i686 -Iqxpdisp -o $@ \
		qxp_vdecl_test.c qxpdisp/qxp_vdecl.c

$(BUILD)/qxp_vdecl9_test.exe: qxp_vdecl9_test.c \
		qxpdisp/qxp_vdecl9.c qxpdisp/qxp_vdecl9.h | $(BUILD)
	$(WINCC) $(CFLAGS) -march=i686 -Iqxpdisp -o $@ \
		qxp_vdecl9_test.c qxpdisp/qxp_vdecl9.c

$(BUILD)/qxp_d3d9_shader_objects_test.exe: \
		qxp_d3d9_shader_objects_test.c \
		qxpdisp/qxp_d3d9_shader_objects.c \
		qxpdisp/qxp_d3d9_shader_objects.h \
		qxpdisp/qxp_d3d9_draw_state.c \
		qxpdisp/qxp_d3d9_draw_state.h \
		qxpdisp/qxp_shader_decode.c qxpdisp/qxp_shader_ir.h \
		qxpdisp/qxp_shader_owner.c qxpdisp/qxp_shader_owner.h \
		qxpdisp/qxp_pshader_glsl.c qxpdisp/qxp_pshader_glsl.h \
		qxpdisp/qxp_vdecl9.c qxpdisp/qxp_vdecl9.h | $(BUILD)
	$(WINCC) $(CFLAGS) -march=i686 -Iqxpdisp -o $@ \
		qxp_d3d9_shader_objects_test.c \
		qxpdisp/qxp_d3d9_shader_objects.c \
		qxpdisp/qxp_d3d9_draw_state.c \
		qxpdisp/qxp_shader_decode.c qxpdisp/qxp_shader_owner.c \
		qxpdisp/qxp_pshader_glsl.c qxpdisp/qxp_vdecl9.c

d3d9-shader-objects-test: $(BUILD)/qxp_d3d9_shader_objects_test.exe
	$(BUILD)/qxp_d3d9_shader_objects_test.exe

$(BUILD)/qxp_d3d9_draw_state_test.exe: \
		qxp_d3d9_draw_state_test.c \
		qxpdisp/qxp_d3d9_draw_state.c \
		qxpdisp/qxp_d3d9_draw_state.h \
		qxpdisp/qxp_d3d9_shader_objects.h | $(BUILD)
	$(WINCC) $(CFLAGS) -march=i686 -Iqxpdisp -o $@ \
		qxp_d3d9_draw_state_test.c \
		qxpdisp/qxp_d3d9_draw_state.c

d3d9-draw-state-test: $(BUILD)/qxp_d3d9_draw_state_test.exe
	$(BUILD)/qxp_d3d9_draw_state_test.exe

$(BUILD)/qxp_d3d9_shader_link_test.exe: \
		qxp_d3d9_shader_link_test.c \
		qxpdisp/qxp_d3d9_shader_link.c \
		qxpdisp/qxp_d3d9_shader_link.h \
		qxpdisp/qxp_shader_decode.c qxpdisp/qxp_shader_ir.h \
		qxpdisp/qxp_vdecl9.c qxpdisp/qxp_vdecl9.h | $(BUILD)
	$(WINCC) $(CFLAGS) -march=i686 -Iqxpdisp -o $@ \
		qxp_d3d9_shader_link_test.c \
		qxpdisp/qxp_d3d9_shader_link.c \
		qxpdisp/qxp_shader_decode.c qxpdisp/qxp_vdecl9.c

d3d9-shader-link-test: $(BUILD)/qxp_d3d9_shader_link_test.exe
	$(BUILD)/qxp_d3d9_shader_link_test.exe

$(BUILD)/qxp_d3d9_draw_execute_test.exe: \
		qxp_d3d9_draw_execute_test.c \
		qxp_d3d_transport.h \
		qxpdisp/qxp_d3d9_draw_execute.c \
		qxpdisp/qxp_d3d9_shader_link.c \
		qxpdisp/qxp_d3d9_draw_execute.h \
		qxpdisp/qxp_d3d9_draw_prepare.h \
		qxpdisp/qxp_positiont_vshader_glsl.h \
		qxpdisp/qxp_d3d9_shader_link.h \
		qxpdisp/qxp_vdecl.h \
		qxpdisp/qxp_vdecl9.c qxpdisp/qxp_vdecl9.h | $(BUILD)
	$(WINCC) $(CFLAGS) -std=c89 -pedantic -march=i686 -Iqxpdisp \
		-o $@ qxp_d3d9_draw_execute_test.c \
		qxpdisp/qxp_d3d9_draw_execute.c qxpdisp/qxp_d3d9_shader_link.c qxpdisp/qxp_vdecl9.c

d3d9-draw-execute-test: $(BUILD)/qxp_d3d9_draw_execute_test.exe
	$(BUILD)/qxp_d3d9_draw_execute_test.exe

$(BUILD)/qxp_d3d9_draw_prepare_test.exe: \
		qxp_d3d9_draw_prepare_test.c \
		qxpdisp/qxp_d3d9_draw_backing.c \
		qxpdisp/qxp_d3d9_draw_backing.h \
		qxpdisp/qxp_d3d9_draw_gate.c \
		qxpdisp/qxp_d3d9_draw_gate.h \
		qxpdisp/qxp_d3d9_draw_prepare.c \
		qxpdisp/qxp_positiont_vshader_glsl.c \
		qxpdisp/qxp_ffpshader_glsl.c \
		qxpdisp/qxp_d3d9_draw_prepare.h \
		qxpdisp/qxp_positiont_vshader_glsl.h \
		qxpdisp/qxp_d3d9_draw_state.c \
		qxpdisp/qxp_d3d9_draw_state.h \
		qxpdisp/qxp_d3d9_shader_objects.c \
		qxpdisp/qxp_d3d9_shader_objects.h \
		qxpdisp/qxp_d3d9_shader_link.c \
		qxpdisp/qxp_d3d9_shader_link.h \
		qxpdisp/qxp_shader_decode.c qxpdisp/qxp_shader_ir.h \
		qxpdisp/qxp_shader_owner.c qxpdisp/qxp_shader_owner.h \
		qxpdisp/qxp_pshader_glsl.c qxpdisp/qxp_pshader_glsl.h \
		qxpdisp/qxp_vshader20_glsl.c qxpdisp/qxp_vshader20_glsl.h \
		qxpdisp/qxp_vdecl9.c qxpdisp/qxp_vdecl9.h | $(BUILD)
	$(WINCC) $(CFLAGS) -std=c89 -pedantic -march=i686 -Iqxpdisp \
		-o $@ qxp_d3d9_draw_prepare_test.c \
		qxpdisp/qxp_d3d9_draw_backing.c \
		qxpdisp/qxp_d3d9_draw_gate.c \
		qxpdisp/qxp_d3d9_draw_prepare.c \
		qxpdisp/qxp_positiont_vshader_glsl.c \
		qxpdisp/qxp_ffpshader_glsl.c \
		qxpdisp/qxp_d3d9_draw_state.c \
		qxpdisp/qxp_d3d9_shader_objects.c \
		qxpdisp/qxp_d3d9_shader_link.c \
		qxpdisp/qxp_shader_decode.c qxpdisp/qxp_shader_owner.c \
		qxpdisp/qxp_pshader_glsl.c qxpdisp/qxp_vshader20_glsl.c \
		qxpdisp/qxp_vdecl9.c

d3d9-draw-prepare-test: $(BUILD)/qxp_d3d9_draw_prepare_test.exe
	$(BUILD)/qxp_d3d9_draw_prepare_test.exe

$(BUILD)/qxp_d3d9_draw_backing_test.exe: \
		qxp_d3d9_draw_backing_test.c \
		qxpdisp/qxp_d3d9_draw_backing.c \
		qxpdisp/qxp_d3d9_draw_backing.h \
		qxpdisp/qxp_d3d9_draw_prepare.h \
		qxpdisp/qxp_positiont_vshader_glsl.h \
		qxpdisp/qxp_d3d9_draw_state.c \
		qxpdisp/qxp_d3d9_draw_state.h \
		qxpdisp/qxp_d3d9_shader_objects.h \
		qxpdisp/qxp_d3d9_shader_link.h | $(BUILD)
	$(WINCC) $(CFLAGS) -std=c89 -pedantic -march=i686 -Iqxpdisp \
		-o $@ qxp_d3d9_draw_backing_test.c \
		qxpdisp/qxp_d3d9_draw_backing.c \
		qxpdisp/qxp_d3d9_draw_state.c

d3d9-draw-backing-test: $(BUILD)/qxp_d3d9_draw_backing_test.exe
	$(BUILD)/qxp_d3d9_draw_backing_test.exe

$(BUILD)/qxp_d3d9_draw_envelope_test.exe: \
		qxp_d3d9_draw_envelope_test.c \
		qxpdisp/qxp_d3d9_draw_envelope.c \
		qxpdisp/qxp_d3d9_draw_envelope.h \
		qxpdisp/qxp_d3d9_draw_backing.h \
		qxpdisp/qxp_d3d9_draw_gate.h \
		qxpdisp/qxp_d3d9_draw_prepare.h | $(BUILD)
	$(WINCC) $(CFLAGS) -std=c89 -pedantic -march=i686 -Iqxpdisp \
		-o $@ qxp_d3d9_draw_envelope_test.c \
		qxpdisp/qxp_d3d9_draw_envelope.c

d3d9-draw-envelope-test: $(BUILD)/qxp_d3d9_draw_envelope_test.exe
	$(BUILD)/qxp_d3d9_draw_envelope_test.exe

$(BUILD)/qxp_d3d9_draw_access_test.exe: \
		qxp_d3d9_draw_access_test.c \
		qxpdisp/qxp_d3d9_draw_access.c \
		qxpdisp/qxp_d3d9_draw_access.h \
		qxpdisp/qxp_d3d9_draw_envelope.c \
		qxpdisp/qxp_d3d9_draw_envelope.h \
		qxpdisp/qxp_d3d9_draw_backing.c \
		qxpdisp/qxp_d3d9_draw_backing.h \
		qxpdisp/qxp_d3d9_draw_gate.c \
		qxpdisp/qxp_d3d9_draw_gate.h \
		qxpdisp/qxp_d3d9_draw_prepare.c \
		qxpdisp/qxp_positiont_vshader_glsl.c \
		qxpdisp/qxp_ffpshader_glsl.c \
		qxpdisp/qxp_d3d9_draw_prepare.h \
		qxpdisp/qxp_positiont_vshader_glsl.h \
		qxpdisp/qxp_d3d9_draw_state.c \
		qxpdisp/qxp_d3d9_draw_state.h \
		qxpdisp/qxp_d3d9_shader_objects.c \
		qxpdisp/qxp_d3d9_shader_objects.h \
		qxpdisp/qxp_d3d9_shader_link.c \
		qxpdisp/qxp_d3d9_shader_link.h \
		qxpdisp/qxp_shader_decode.c qxpdisp/qxp_shader_ir.h \
		qxpdisp/qxp_shader_owner.c qxpdisp/qxp_shader_owner.h \
		qxpdisp/qxp_pshader_glsl.c qxpdisp/qxp_pshader_glsl.h \
		qxpdisp/qxp_vshader20_glsl.c \
		qxpdisp/qxp_vshader20_glsl.h \
		qxpdisp/qxp_vdecl9.c qxpdisp/qxp_vdecl9.h | $(BUILD)
	$(WINCC) $(CFLAGS) -std=c89 -pedantic -Wall -Wextra -Werror \
		-march=i686 -Iqxpdisp -o $@ qxp_d3d9_draw_access_test.c \
		qxpdisp/qxp_d3d9_draw_access.c \
		qxpdisp/qxp_d3d9_draw_envelope.c \
		qxpdisp/qxp_d3d9_draw_backing.c \
		qxpdisp/qxp_d3d9_draw_gate.c \
		qxpdisp/qxp_d3d9_draw_prepare.c \
		qxpdisp/qxp_positiont_vshader_glsl.c \
		qxpdisp/qxp_ffpshader_glsl.c \
		qxpdisp/qxp_d3d9_draw_state.c \
		qxpdisp/qxp_d3d9_shader_objects.c \
		qxpdisp/qxp_d3d9_shader_link.c \
		qxpdisp/qxp_shader_decode.c qxpdisp/qxp_shader_owner.c \
		qxpdisp/qxp_pshader_glsl.c qxpdisp/qxp_vshader20_glsl.c \
		qxpdisp/qxp_vdecl9.c

d3d9-draw-access-test: $(BUILD)/qxp_d3d9_draw_access_test.exe
	$(BUILD)/qxp_d3d9_draw_access_test.exe

$(BUILD)/qxp_d3d9_draw_capture_test.exe: \
		qxp_d3d9_draw_capture_test.c \
		qxpdisp/qxp_d3d9_draw_capture.c \
		qxpdisp/qxp_d3d9_draw_capture.h \
		qxpdisp/qxp_d3d9_draw_access.c \
		qxpdisp/qxp_d3d9_draw_access.h \
		qxpdisp/qxp_d3d9_draw_envelope.c \
		qxpdisp/qxp_d3d9_draw_envelope.h \
		qxpdisp/qxp_d3d9_draw_backing.c \
		qxpdisp/qxp_d3d9_draw_backing.h \
		qxpdisp/qxp_d3d9_draw_gate.c \
		qxpdisp/qxp_d3d9_draw_gate.h \
		qxpdisp/qxp_d3d9_draw_prepare.c \
		qxpdisp/qxp_positiont_vshader_glsl.c \
		qxpdisp/qxp_ffpshader_glsl.c \
		qxpdisp/qxp_d3d9_draw_prepare.h \
		qxpdisp/qxp_positiont_vshader_glsl.h \
		qxpdisp/qxp_d3d9_draw_state.c \
		qxpdisp/qxp_d3d9_draw_state.h \
		qxpdisp/qxp_d3d9_shader_objects.c \
		qxpdisp/qxp_d3d9_shader_objects.h \
		qxpdisp/qxp_d3d9_shader_link.c \
		qxpdisp/qxp_d3d9_shader_link.h \
		qxpdisp/qxp_shader_decode.c qxpdisp/qxp_shader_ir.h \
		qxpdisp/qxp_shader_owner.c qxpdisp/qxp_shader_owner.h \
		qxpdisp/qxp_pshader_glsl.c qxpdisp/qxp_pshader_glsl.h \
		qxpdisp/qxp_vshader20_glsl.c \
		qxpdisp/qxp_vshader20_glsl.h \
		qxpdisp/qxp_vdecl9.c qxpdisp/qxp_vdecl9.h | $(BUILD)
	$(WINCC) $(CFLAGS) -std=c89 -pedantic -Wall -Wextra -Werror \
		-march=i686 -Iqxpdisp -o $@ qxp_d3d9_draw_capture_test.c \
		qxpdisp/qxp_d3d9_draw_capture.c \
		qxpdisp/qxp_d3d9_draw_access.c \
		qxpdisp/qxp_d3d9_draw_envelope.c \
		qxpdisp/qxp_d3d9_draw_backing.c \
		qxpdisp/qxp_d3d9_draw_gate.c \
		qxpdisp/qxp_d3d9_draw_prepare.c \
		qxpdisp/qxp_positiont_vshader_glsl.c \
		qxpdisp/qxp_ffpshader_glsl.c \
		qxpdisp/qxp_d3d9_draw_state.c \
		qxpdisp/qxp_d3d9_shader_objects.c \
		qxpdisp/qxp_d3d9_shader_link.c \
		qxpdisp/qxp_shader_decode.c qxpdisp/qxp_shader_owner.c \
		qxpdisp/qxp_pshader_glsl.c qxpdisp/qxp_vshader20_glsl.c \
		qxpdisp/qxp_vdecl9.c

d3d9-draw-capture-test: $(BUILD)/qxp_d3d9_draw_capture_test.exe
	$(BUILD)/qxp_d3d9_draw_capture_test.exe

$(BUILD)/qxp_d3d9_draw_lifetime_test.exe: \
		qxp_d3d9_draw_lifetime_test.c \
		qxpdisp/qxp_d3d9_draw_lifetime.c \
		qxpdisp/qxp_d3d9_draw_lifetime.h | $(BUILD)
	$(WINCC) $(CFLAGS) -std=c89 -pedantic -Wall -Wextra -Werror \
		-march=i686 -Iqxpdisp -o $@ qxp_d3d9_draw_lifetime_test.c \
		qxpdisp/qxp_d3d9_draw_lifetime.c

d3d9-draw-lifetime-test: $(BUILD)/qxp_d3d9_draw_lifetime_test.exe
	$(BUILD)/qxp_d3d9_draw_lifetime_test.exe

$(BUILD)/qxp_d3d9_draw_source_test.exe: \
		qxp_d3d9_draw_source_test.c \
		qxpdisp/qxp_d3d9_draw_source.c \
		qxpdisp/qxp_d3d9_draw_source.h \
		qxpdisp/qxp_d3d9_draw_lifetime.c \
		qxpdisp/qxp_d3d9_draw_lifetime.h \
		qxpdisp/qxp_d3d9_draw_backing.h \
		qxpdisp/qxp_d3d_texblt.c \
		qxpdisp/qxp_d3d_texblt.h | $(BUILD)
	$(WINCC) $(CFLAGS) -std=c89 -pedantic -Wall -Wextra -Werror \
		-march=i686 -Iqxpdisp -o $@ qxp_d3d9_draw_source_test.c \
		qxpdisp/qxp_d3d9_draw_source.c \
		qxpdisp/qxp_d3d9_draw_lifetime.c \
		qxpdisp/qxp_d3d_texblt.c

d3d9-draw-source-test: $(BUILD)/qxp_d3d9_draw_source_test.exe
	$(BUILD)/qxp_d3d9_draw_source_test.exe

$(BUILD)/qxp_d3d_texblt_test.exe: qxp_d3d_texblt_test.c \
		qxpdisp/qxp_d3d_texblt.c qxpdisp/qxp_d3d_texblt.h | $(BUILD)
	$(WINCC) $(CFLAGS) -std=c89 -pedantic -march=i686 -Iqxpdisp -o $@ \
		qxp_d3d_texblt_test.c qxpdisp/qxp_d3d_texblt.c

d3d-texblt-test: $(BUILD)/qxp_d3d_texblt_test.exe
	$(BUILD)/qxp_d3d_texblt_test.exe

$(BUILD)/qxp_pshader_glsl_test.exe: \
		qxp_pshader_glsl_test.c \
		qxpdisp/qxp_pshader_glsl.c \
		qxpdisp/qxp_pshader_glsl.h \
		qxpdisp/qxp_shader_decode.c qxpdisp/qxp_shader_ir.h | $(BUILD)
	$(WINCC) $(CFLAGS) -march=i686 -Iqxpdisp -o $@ \
		qxp_pshader_glsl_test.c qxpdisp/qxp_pshader_glsl.c \
		qxpdisp/qxp_shader_decode.c

$(BUILD)/qxp_positiont_vshader_glsl_test.exe: \
		qxp_positiont_vshader_glsl_test.c \
		qxpdisp/qxp_positiont_vshader_glsl.c \
		qxpdisp/qxp_positiont_vshader_glsl.h \
		qxpdisp/qxp_vshader20_glsl.h | $(BUILD)
	$(WINCC) $(CFLAGS) -march=i686 -Iqxpdisp -o $@ \
		qxp_positiont_vshader_glsl_test.c qxpdisp/qxp_positiont_vshader_glsl.c

positiont-vshader-glsl-test: $(BUILD)/qxp_positiont_vshader_glsl_test.exe
	$(BUILD)/qxp_positiont_vshader_glsl_test.exe

$(BUILD)/qxp_ffpshader_glsl_test.exe: \
		qxp_ffpshader_glsl_test.c \
		qxpdisp/qxp_ffpshader_glsl.c \
		qxpdisp/qxp_ffpshader_glsl.h \
		qxpmp/qxp_texture_op.h qxpdisp/qxp_shader_ir.h | $(BUILD)
	$(WINCC) $(CFLAGS) -march=i686 -Iqxpdisp -o $@ \
		qxp_ffpshader_glsl_test.c qxpdisp/qxp_ffpshader_glsl.c

ffpshader-glsl-test: $(BUILD)/qxp_ffpshader_glsl_test.exe
	$(BUILD)/qxp_ffpshader_glsl_test.exe

$(BUILD)/qxp_ps20_coverage.glsl: $(BUILD)/qxp_pshader_glsl_test.exe
	$(BUILD)/qxp_pshader_glsl_test.exe --emit-glsl $@

$(BUILD)/qxp_ps20_noncentroid.glsl: \
		$(BUILD)/qxp_pshader_glsl_test.exe
	$(BUILD)/qxp_pshader_glsl_test.exe --emit-noncentroid-glsl $@

ps20-glsl-smoke: $(BUILD)/qxp_glsl_compat_test.exe \
		$(BUILD)/qxp_ps20_coverage.glsl
	$(BUILD)/qxp_glsl_compat_test.exe --ps20-glsl \
		$(BUILD)/qxp_ps20_coverage.glsl

$(BUILD)/qxp_vshader20_glsl_test.exe: \
		qxp_vshader20_glsl_test.c \
		qxpdisp/qxp_vshader20_glsl.c \
		qxpdisp/qxp_vshader20_glsl.h \
		qxpdisp/qxp_shader_decode.c qxpdisp/qxp_shader_ir.h | $(BUILD)
	$(WINCC) $(CFLAGS) -march=i686 -Iqxpdisp -o $@ \
		qxp_vshader20_glsl_test.c qxpdisp/qxp_vshader20_glsl.c \
		qxpdisp/qxp_shader_decode.c

$(BUILD)/qxp_vs20_coverage.glsl: $(BUILD)/qxp_vshader20_glsl_test.exe
	$(BUILD)/qxp_vshader20_glsl_test.exe --emit-glsl $@

$(BUILD)/qxp_vs20_noncentroid.glsl: \
		$(BUILD)/qxp_vshader20_glsl_test.exe
	$(BUILD)/qxp_vshader20_glsl_test.exe --emit-noncentroid-glsl $@

$(BUILD)/qxp_vs20_special.glsl: \
		$(BUILD)/qxp_vshader20_glsl_test.exe
	$(BUILD)/qxp_vshader20_glsl_test.exe --emit-special-glsl $@

$(BUILD)/qxp_vs20_arithmetic.glsl: \
		$(BUILD)/qxp_vshader20_glsl_test.exe
	$(BUILD)/qxp_vshader20_glsl_test.exe --emit-arithmetic-glsl $@

$(BUILD)/qxp_vs20_fog.glsl: $(BUILD)/qxp_vshader20_glsl_test.exe
	$(BUILD)/qxp_vshader20_glsl_test.exe --emit-fog-glsl $@

$(BUILD)/qxp_vs20_nofog.glsl: $(BUILD)/qxp_vshader20_glsl_test.exe
	$(BUILD)/qxp_vshader20_glsl_test.exe --emit-nofog-glsl $@

$(BUILD)/qxp_positiont.glsl: \
		$(BUILD)/qxp_positiont_vshader_glsl_test.exe
	$(BUILD)/qxp_positiont_vshader_glsl_test.exe --emit-glsl $@

vs20-glsl-smoke: $(BUILD)/qxp_glsl_compat_test.exe \
		$(BUILD)/qxp_vs20_coverage.glsl \
		$(BUILD)/qxp_vs20_special.glsl \
		$(BUILD)/qxp_vs20_arithmetic.glsl \
		$(BUILD)/qxp_vs20_fog.glsl \
		$(BUILD)/qxp_vs20_nofog.glsl \
		$(BUILD)/qxp_positiont.glsl
	$(BUILD)/qxp_glsl_compat_test.exe --vs20-glsl \
		$(BUILD)/qxp_vs20_coverage.glsl
	$(BUILD)/qxp_glsl_compat_test.exe --vs20-special \
		$(BUILD)/qxp_vs20_special.glsl
	$(BUILD)/qxp_glsl_compat_test.exe --vs20-arithmetic \
		$(BUILD)/qxp_vs20_arithmetic.glsl
	$(BUILD)/qxp_glsl_compat_test.exe --vs20-fog \
		$(BUILD)/qxp_vs20_fog.glsl \
		$(BUILD)/qxp_vs20_nofog.glsl
	$(BUILD)/qxp_glsl_compat_test.exe --positiont-glsl \
		$(BUILD)/qxp_positiont.glsl

sm20-pair-smoke: $(BUILD)/qxp_glsl_compat_test.exe \
		$(BUILD)/qxp_vs20_coverage.glsl \
		$(BUILD)/qxp_ps20_coverage.glsl \
		$(BUILD)/qxp_vs20_noncentroid.glsl \
		$(BUILD)/qxp_ps20_noncentroid.glsl
	$(BUILD)/qxp_glsl_compat_test.exe --sm20-pair \
		$(BUILD)/qxp_vs20_coverage.glsl \
		$(BUILD)/qxp_ps20_coverage.glsl
	$(BUILD)/qxp_glsl_compat_test.exe --sm20-pair \
		$(BUILD)/qxp_vs20_noncentroid.glsl \
		$(BUILD)/qxp_ps20_noncentroid.glsl

$(BUILD)/qxp_hwtl_test.exe: qxp_hwtl_test.c \
		qxpdisp/qxp_hwtl.c qxpdisp/qxp_hwtl.h qxp_d3d_transport.h | $(BUILD)
	$(WINCC) $(CFLAGS) -march=i686 -Iqxpdisp -o $@ \
		qxp_hwtl_test.c qxpdisp/qxp_hwtl.c

$(BUILD)/qxp_vertex_test.exe: qxp_vertex_test.c \
		qxpdisp/qxp_vertex.c qxpdisp/qxp_vertex.h qxp_d3d_transport.h | $(BUILD)
	$(WINCC) $(CFLAGS) -march=i686 -Iqxpdisp -o $@ \
		qxp_vertex_test.c qxpdisp/qxp_vertex.c

$(BUILD)/qxp_profile_timeline_test.exe: qxp_profile_timeline_test.c \
		qxpdisp/qxp_profile_timeline.h | $(BUILD)
	$(WINCC) $(CFLAGS) -march=i686 -Iqxpdisp -o $@ $<

$(BUILD)/qxp_d3d9_sampler_test.exe: qxp_d3d9_sampler_test.c \
		qxpdisp/qxp_d3d9_sampler.c qxpdisp/qxp_d3d9_sampler.h \
		qxp_d3d_transport.h | $(BUILD)
	$(WINCC) $(CFLAGS) -march=i686 -Iqxpdisp -o $@ \
		qxp_d3d9_sampler_test.c qxpdisp/qxp_d3d9_sampler.c

$(BUILD)/qxp_packet_test.exe: qxp_packet_test.c \
		qxpmp/qxp_packet.c qxpmp/qxp_packet.h qxp_d3d_transport.h | $(BUILD)
	$(WINCC) $(CFLAGS) -march=i686 -DQXP_PACKET_STANDALONE -Iqxpmp -o $@ \
		qxp_packet_test.c qxpmp/qxp_packet.c

$(BUILD)/qxp_float_test.exe: qxp_float_test.c \
		qxpmp/qxp_float.c qxpmp/qxp_float.h | $(BUILD)
	$(WINCC) $(CFLAGS) -march=i686 -DQXP_FLOAT_STANDALONE -Iqxpmp -o $@ \
		qxp_float_test.c qxpmp/qxp_float.c

$(BUILD)/qxp_program_test.exe: qxp_program_test.c \
		qxpmp/qxp_program.c qxpmp/qxp_program.h qxp_d3d_transport.h | $(BUILD)
	$(WINCC) $(CFLAGS) -march=i686 -DQXP_PROGRAM_STANDALONE -Iqxpmp -o $@ \
		qxp_program_test.c qxpmp/qxp_program.c

$(BUILD)/qxp_texture_op_test.exe: qxp_texture_op_test.c \
		qxpmp/qxp_texture_op.c qxpmp/qxp_texture_op.h | $(BUILD)
	$(WINCC) $(CFLAGS) -march=i686 -DQXP_TEXTURE_OP_STANDALONE \
		-Iqxpmp -o $@ qxp_texture_op_test.c qxpmp/qxp_texture_op.c

$(BUILD)/qxp_context_lifetime_test.exe: qxp_context_lifetime_test.c \
		qxpdisp/qxp_context_lifetime.c \
		qxpdisp/qxp_context_lifetime.h | $(BUILD)
	$(WINCC) $(CFLAGS) -march=i686 -Iqxpdisp -o $@ \
		qxp_context_lifetime_test.c qxpdisp/qxp_context_lifetime.c

$(BUILD)/qxp_shader_owner_test.exe: qxp_shader_owner_test.c \
		qxpdisp/qxp_shader_owner.c qxpdisp/qxp_shader_owner.h | $(BUILD)
	$(WINCC) $(CFLAGS) -march=i686 -Iqxpdisp -o $@ \
		qxp_shader_owner_test.c qxpdisp/qxp_shader_owner.c

$(BUILD)/qxpcoin2_rc.o: qxpcoin2.rc $(XPD_VERSION_H) | $(BUILD)
	$(WINDRES) $(XPD_VERSION_INC) -O coff -o $@ $<

$(BUILD)/qxpcoin2.dll: qxpcoinst.c $(BUILD)/qxpcoin2_rc.o | $(BUILD)
	$(WINCC) $(CFLAGS) -march=i686 -shared -static-libgcc \
		-Wl,--subsystem,windows:5.01 \
		-Wl,--major-os-version,5,--minor-os-version,1 \
		-Wl,--kill-at -o $@ $^ -ladvapi32 -lsetupapi

$(BUILD)/qxpcoinst_test.exe: qxpcoinst_test.c qxpcoinst.c | $(BUILD)
	$(WINCC) $(CFLAGS) -march=i686 -o $@ $<

$(BUILD)/qxp_glsl_compat_test.exe: qxp_glsl_compat_test.c | $(BUILD)
	$(WINCC) $(CFLAGS) -march=i686 -o $@ $< \
		-lopengl32 -lgdi32 -luser32

$(BUILD)/qxp_scalable_point.glsl: qxpmp/qxpmp.c \
		extract-glsl-source.py | $(BUILD)
	python3 extract-glsl-source.py $< $@

glsl-compat-smoke: $(BUILD)/qxp_glsl_compat_test.exe \
		$(BUILD)/qxp_scalable_point.glsl
	$(BUILD)/qxp_glsl_compat_test.exe $(BUILD)/qxp_scalable_point.glsl

$(BUILD)/qxp_vshader_smoke.arbvp: $(BUILD)/qxp_vshader_test.exe
	$(BUILD)/qxp_vshader_test.exe --emit-arbvp $@

arbvp-compat-smoke: $(BUILD)/qxp_glsl_compat_test.exe \
		$(BUILD)/qxp_vshader_smoke.arbvp
	$(BUILD)/qxp_glsl_compat_test.exe --arbvp \
		$(BUILD)/qxp_vshader_smoke.arbvp

$(BUILD)/qxp_compact_transformed.arbvp: qxpmp/qxpmp.c \
		extract-glsl-source.py | $(BUILD)
	python3 extract-glsl-source.py $< $@ \
		qxp_d3d_compact_transformed_program

compact-arbvp-compat-smoke: $(BUILD)/qxp_glsl_compat_test.exe \
		$(BUILD)/qxp_compact_transformed.arbvp
	$(BUILD)/qxp_glsl_compat_test.exe --arbvp \
		$(BUILD)/qxp_compact_transformed.arbvp

$(BUILD)/qxp_vshader_clip.glsl: $(BUILD)/qxp_vshader_test.exe
	$(BUILD)/qxp_vshader_test.exe --emit-clip-glsl $@

$(BUILD)/qxp_vshader_clip_visual.glsl: $(BUILD)/qxp_vshader_test.exe
	$(BUILD)/qxp_vshader_test.exe --emit-clip-visual-glsl $@

clip-glsl-compat-smoke: $(BUILD)/qxp_glsl_compat_test.exe \
		$(BUILD)/qxp_vshader_clip.glsl \
		$(BUILD)/qxp_vshader_clip_visual.glsl
	$(BUILD)/qxp_glsl_compat_test.exe --clip-glsl \
		$(BUILD)/qxp_vshader_clip.glsl
	$(BUILD)/qxp_glsl_compat_test.exe --clip-glsl-visual \
		$(BUILD)/qxp_vshader_clip_visual.glsl

surface-copy-smoke: $(BUILD)/qxp_glsl_compat_test.exe
	$(BUILD)/qxp_glsl_compat_test.exe --surface-copy

surface-readback-smoke: $(BUILD)/qxp_glsl_compat_test.exe
	$(BUILD)/qxp_glsl_compat_test.exe --surface-readback

check-arbvp:
	sh check-arbvp-opcodes.sh qxpmp/qxpmp.c

# Requires a Windows desktop OpenGL renderer; keep this explicit rather than
# making the portable/headless `test` target depend on WGL.
preinstall-test: test ps20-glsl-smoke vs20-glsl-smoke \
	sm20-pair-smoke glsl-compat-smoke arbvp-compat-smoke \
	clip-glsl-compat-smoke surface-copy-smoke surface-readback-smoke

test: all check-arbvp
	$(BUILD)/qxpescape_test
	$(BUILD)/qxp_shader_decode_test.exe
	$(BUILD)/qxp_shader_test.exe
	$(BUILD)/qxp_vshader_test.exe
	$(BUILD)/qxp_vdecl_test.exe
	$(BUILD)/qxp_vdecl9_test.exe
	$(BUILD)/qxp_d3d9_shader_objects_test.exe
	$(BUILD)/qxp_d3d9_draw_state_test.exe
	$(BUILD)/qxp_d3d9_shader_link_test.exe
	$(BUILD)/qxp_d3d9_draw_prepare_test.exe
	$(BUILD)/qxp_d3d9_draw_execute_test.exe
	$(BUILD)/qxp_d3d9_draw_backing_test.exe
	$(BUILD)/qxp_d3d9_draw_envelope_test.exe
	$(BUILD)/qxp_d3d9_draw_access_test.exe
	$(BUILD)/qxp_d3d9_draw_capture_test.exe
	$(BUILD)/qxp_d3d9_draw_lifetime_test.exe
	$(BUILD)/qxp_d3d9_draw_source_test.exe
	$(BUILD)/qxp_d3d_texblt_test.exe
	$(BUILD)/qxp_pshader_glsl_test.exe
	$(BUILD)/qxp_ffpshader_glsl_test.exe
	$(BUILD)/qxp_positiont_vshader_glsl_test.exe
	$(BUILD)/qxp_vshader20_glsl_test.exe
	$(BUILD)/qxp_hwtl_test.exe
	$(BUILD)/qxp_vertex_test.exe
	$(BUILD)/qxp_profile_timeline_test.exe
	$(BUILD)/qxp_d3d9_sampler_test.exe
	$(BUILD)/qxp_packet_test.exe
	$(BUILD)/qxp_float_test.exe
	$(BUILD)/qxp_program_test.exe
	$(BUILD)/qxp_texture_op_test.exe
	$(BUILD)/qxp_context_lifetime_test.exe
	$(BUILD)/qxp_shader_owner_test.exe
	$(BUILD)/qxpcoinst_test.exe
	python3 validate_inf.py

clean:
	rm -rf $(BUILD)
