#include <stdio.h>
#include <string.h>

#include "qxpdisp/qxp_d3d9_draw_execute.h"

static int qxp_failures;

static void qxp_check(int condition, const char *name)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", name);
        ++qxp_failures;
    }
}

static DWORD qxp_popcount(DWORD value)
{
    DWORD count;

    count = 0;
    while (value) {
        count += value & 1UL;
        value >>= 1;
    }
    return count;
}

static void qxp_zero_views(QXP_VERTEX_STREAM_VIEW views[16])
{
    memset(views, 0, sizeof(QXP_VERTEX_STREAM_VIEW) * 16);
}

static int qxp_make_prepared(
    QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_VERTEX_ELEMENT9 *elements, DWORD element_count,
    DWORD input_mask, const DWORD usages[16],
    const DWORD usage_indices[16],
    const QXP_VERTEX_STREAM_VIEW views[16])
{
    QXP_VERTEX_DECLARATION9_ERROR error;
    DWORD input_index;
    DWORD stream;

    memset(prepared, 0, sizeof(*prepared));
    memset(&error, 0, sizeof(error));
    if (!QxpParseVertexDeclaration9(
            elements, element_count, &prepared->declaration, &error))
        return 0;
    prepared->valid = TRUE;
    prepared->mode = QXP_D3D9_DRAW_PREPARED_MODE_PROGRAMMABLE;
    prepared->vertex_metadata.input_mask = input_mask;
    prepared->vertex_metadata.declaration_count = qxp_popcount(input_mask);
    prepared->vertex_metadata.writes_position = TRUE;
    prepared->vertex_metadata.position_component_mask = 15;
    prepared->vertex_link.mode = QXP_D3D9_VERTEX_LINK_MODE_PROGRAMMABLE;
    prepared->vertex_link.input_mask = input_mask;
    prepared->vertex_link.input_count = qxp_popcount(input_mask);
    for (input_index = 0; input_index < 16; ++input_index)
        prepared->vertex_link.inputs[input_index].declaration_element =
            QXP_D3D9_VERTEX_LINK_INDEX_NONE;

    for (input_index = 0; input_index < 16; ++input_index) {
        QXP_D3D9_VERTEX_INPUT_LINK *input;
        DWORD input_bit;
        DWORD element_index;
        int found;

        input_bit = 1UL << input_index;
        if (!(input_mask & input_bit)) continue;
        prepared->vertex_metadata.input_usage[input_index] =
            usages[input_index];
        prepared->vertex_metadata.input_usage_index[input_index] =
            usage_indices[input_index];
        prepared->vertex_metadata.input_component_mask[input_index] = 15;
        input = &prepared->vertex_link.inputs[input_index];
        input->declared = TRUE;
        input->usage = usages[input_index];
        input->usage_index = usage_indices[input_index];
        input->destination_register = input_index;
        found = 0;
        for (element_index = 0; element_index < element_count;
             ++element_index) {
            const QXP_VERTEX_DECLARATION9_ELEMENT *element;
            DWORD end;

            element = &prepared->declaration.elements[element_index];
            if ((DWORD)element->usage != usages[input_index] ||
                (DWORD)element->usage_index != usage_indices[input_index])
                continue;
            input->matched = TRUE;
            input->declaration_element = element_index;
            input->source_stream = element->stream;
            input->source_offset = element->offset;
            input->source_type = element->type;
            input->source_method = element->method;
            input->source_byte_size = element->byte_size;
            prepared->vertex_link.matched_input_mask |= input_bit;
            prepared->vertex_link.matched_stream_mask |=
                1UL << element->stream;
            prepared->vertex_link.matched_element_mask |=
                ((ULONGLONG)1) << element_index;
            end = (DWORD)element->offset + element->byte_size;
            if (end > prepared->vertex_link.required_stream_spans
                          [element->stream])
                prepared->vertex_link.required_stream_spans
                    [element->stream] = end;
            ++prepared->vertex_link.matched_input_count;
            found = 1;
            break;
        }
        if (!found) {
            prepared->vertex_link.undefined_input_mask |= input_bit;
            ++prepared->vertex_link.undefined_input_count;
        }
    }
    prepared->vertex_link.valid = TRUE;
    prepared->undefined_vertex_input_mask =
        prepared->vertex_link.undefined_input_mask;
    prepared->required_stream_mask =
        prepared->vertex_link.matched_stream_mask;
    memcpy(prepared->required_stream_spans,
           prepared->vertex_link.required_stream_spans,
           sizeof(prepared->required_stream_spans));
    for (stream = 0; stream < 16; ++stream) {
        DWORD bit;

        bit = 1UL << stream;
        if (!(prepared->required_stream_mask & bit)) continue;
        prepared->raw_state.streams[stream].handle = 0x100UL + stream;
        prepared->raw_state.streams[stream].stride = views[stream].stride;
        prepared->raw_state.streams[stream].divider = 1;
    }
    return 1;
}

static void qxp_test_all_types_and_color(void)
{
    QXP_VERTEX_ELEMENT9 elements[8];
    QXP_VERTEX_STREAM_VIEW views[16];
    QXP_D3D9_DRAW_PREPARED prepared;
    QXP_D3D9_PROGRAM_VERTEX_LAYOUT layout;
    BYTE source[120];
    BYTE output[60];
    DWORD usages[16];
    DWORD usage_indices[16];
    static const BYTE sizes[8] = { 4, 8, 12, 16, 4, 4, 4, 8 };
    DWORD index;
    DWORD offset;

    memset(elements, 0, sizeof(elements));
    memset(usages, 0, sizeof(usages));
    memset(usage_indices, 0, sizeof(usage_indices));
    qxp_zero_views(views);
    offset = 0;
    for (index = 0; index < 8; ++index) {
        elements[index].stream = 0;
        elements[index].offset = (WORD)offset;
        elements[index].type = (BYTE)index;
        elements[index].method = QXP_VERTEX_DECLMETHOD9_DEFAULT;
        elements[index].usage = QXP_VERTEX_DECLUSAGE9_TEXCOORD;
        elements[index].usage_index = (BYTE)index;
        usages[index] = QXP_VERTEX_DECLUSAGE9_TEXCOORD;
        usage_indices[index] = index;
        offset += sizes[index];
    }
    for (index = 0; index < sizeof(source); ++index)
        source[index] = (BYTE)(index + 1);
    views[0].base = source;
    views[0].bytes = sizeof(source);
    views[0].stride = offset;
    views[0].wraps = FALSE;
    qxp_check(qxp_make_prepared(
                  &prepared, elements, 8, 0xffUL,
                  usages, usage_indices, views),
              "all-types fixture parses");
    qxp_check(QxpD3d9BuildProgramVertexLayout(
                  &prepared, views, &layout),
              "all advertised base types build");
    qxp_check(layout.valid == TRUE && layout.input_mask == 0xffUL &&
                  layout.used_inputs == 0xffUL &&
                  layout.vertex_stride == offset,
              "all-types layout masks and stride");
    offset = 0;
    for (index = 0; index < 8; ++index) {
        qxp_check(layout.elements[index].format == index &&
                      layout.elements[index].byte_offset == offset,
                  "base type maps directly and tightly");
        offset += sizes[index];
    }
    memset(output, 0xcc, sizeof(output));
    qxp_check(QxpD3d9GatherProgramVertex(
                  &prepared, views, &layout, 1, output, sizeof(output)),
              "all-types second vertex gathers");
    qxp_check(!memcmp(output, source + 60, 40),
              "non-color formats preserve leading raw bytes");
    qxp_check(output[40] == source[102] && output[41] == source[101] &&
                  output[42] == source[100] && output[43] == source[103],
              "D3DCOLOR canonicalizes BGRA to RGBA");
    qxp_check(!memcmp(output + 44, source + 104, 16),
              "non-color formats preserve trailing raw bytes");
}

static void qxp_test_multistream_register_order(void)
{
    QXP_VERTEX_ELEMENT9 elements[3];
    QXP_VERTEX_STREAM_VIEW views[16];
    QXP_D3D9_DRAW_PREPARED prepared;
    QXP_D3D9_PROGRAM_VERTEX_LAYOUT layout;
    BYTE stream0[48];
    BYTE stream1[40];
    BYTE stream2[32];
    BYTE output[20];
    BYTE expected[20];
    DWORD usages[16];
    DWORD usage_indices[16];
    DWORD index;

    memset(elements, 0, sizeof(elements));
    memset(usages, 0, sizeof(usages));
    memset(usage_indices, 0, sizeof(usage_indices));
    qxp_zero_views(views);
    for (index = 0; index < sizeof(stream0); ++index)
        stream0[index] = (BYTE)(0x10 + index);
    for (index = 0; index < sizeof(stream1); ++index)
        stream1[index] = (BYTE)(0x50 + index);
    for (index = 0; index < sizeof(stream2); ++index)
        stream2[index] = (BYTE)(0x90 + index);

    elements[0].stream = 2; elements[0].offset = 12;
    elements[0].type = QXP_VERTEX_DECLTYPE9_FLOAT1;
    elements[0].method = QXP_VERTEX_DECLMETHOD9_DEFAULT;
    elements[0].usage = QXP_VERTEX_DECLUSAGE9_TEXCOORD;
    elements[0].usage_index = 4;
    elements[1].stream = 1; elements[1].offset = 4;
    elements[1].type = QXP_VERTEX_DECLTYPE9_FLOAT2;
    elements[1].method = QXP_VERTEX_DECLMETHOD9_DEFAULT;
    elements[1].usage = QXP_VERTEX_DECLUSAGE9_POSITION;
    elements[1].usage_index = 0;
    elements[2].stream = 0; elements[2].offset = 8;
    elements[2].type = QXP_VERTEX_DECLTYPE9_FLOAT2;
    elements[2].method = QXP_VERTEX_DECLMETHOD9_DEFAULT;
    elements[2].usage = QXP_VERTEX_DECLUSAGE9_TEXCOORD;
    elements[2].usage_index = 2;
    usages[0] = QXP_VERTEX_DECLUSAGE9_POSITION;
    usage_indices[0] = 0;
    usages[2] = QXP_VERTEX_DECLUSAGE9_TEXCOORD;
    usage_indices[2] = 2;
    usages[4] = QXP_VERTEX_DECLUSAGE9_TEXCOORD;
    usage_indices[4] = 4;
    views[0].base = stream0; views[0].bytes = sizeof(stream0);
    views[0].stride = 16;
    views[1].base = stream1; views[1].bytes = sizeof(stream1);
    views[1].stride = 16;
    views[2].base = stream2; views[2].bytes = sizeof(stream2);
    views[2].stride = 16;

    qxp_check(qxp_make_prepared(
                  &prepared, elements, 3, 0x15UL,
                  usages, usage_indices, views),
              "multistream fixture parses");
    prepared.raw_state.streams[0].offset = 4;
    prepared.raw_state.streams[1].offset = 4;
    prepared.raw_state.streams[0].source2 = TRUE;
    prepared.raw_state.streams[1].source2 = TRUE;
    qxp_check(QxpD3d9BuildProgramVertexLayout(
                  &prepared, views, &layout),
              "multistream layout builds");
    qxp_check(layout.vertex_stride == 20 &&
                  layout.elements[0].byte_offset == 0 &&
                  layout.elements[2].byte_offset == 8 &&
                  layout.elements[4].byte_offset == 16,
              "attributes pack by v-register order");
    memcpy(expected, stream1 + 24, 8);
    memcpy(expected + 8, stream0 + 28, 8);
    memcpy(expected + 16, stream2 + 28, 4);
    qxp_check(QxpD3d9GatherProgramVertex(
                  &prepared, views, &layout, 1, output, sizeof(output)) &&
                  !memcmp(output, expected, sizeof(output)),
              "multistream binding/declaration offsets gather exactly");
}

static void qxp_test_missing_inputs(void)
{
    QXP_VERTEX_ELEMENT9 element;
    QXP_VERTEX_STREAM_VIEW views[16];
    QXP_D3D9_DRAW_PREPARED prepared;
    QXP_D3D9_PROGRAM_VERTEX_LAYOUT layout;
    BYTE source[4] = { 1, 2, 3, 4 };
    BYTE output[20];
    DWORD usages[16];
    DWORD usage_indices[16];
    DWORD index;

    memset(&element, 0, sizeof(element));
    memset(usages, 0, sizeof(usages));
    memset(usage_indices, 0, sizeof(usage_indices));
    qxp_zero_views(views);
    element.stream = 0;
    element.type = QXP_VERTEX_DECLTYPE9_FLOAT1;
    element.method = QXP_VERTEX_DECLMETHOD9_DEFAULT;
    element.usage = QXP_VERTEX_DECLUSAGE9_TEXCOORD;
    element.usage_index = 2;
    views[0].base = source; views[0].bytes = sizeof(source);
    views[0].stride = sizeof(source); views[0].wraps = FALSE;
    usages[0] = QXP_VERTEX_DECLUSAGE9_NORMAL;
    usages[2] = QXP_VERTEX_DECLUSAGE9_TEXCOORD;
    usage_indices[2] = 2;
    usages[5] = QXP_VERTEX_DECLUSAGE9_COLOR;
    usage_indices[5] = 1;
    qxp_check(qxp_make_prepared(
                  &prepared, &element, 1, 0x25UL,
                  usages, usage_indices, views),
              "missing-input fixture parses");
    qxp_check(QxpD3d9BuildProgramVertexLayout(
                  &prepared, views, &layout),
              "missing inputs build");
    qxp_check(layout.used_inputs == 0x25UL &&
                  layout.input_mask == 0x05UL &&
                  layout.vertex_stride == 20 &&
                  layout.elements[0].format ==
                      QXP_D3D_VERTEX_FORMAT_FLOAT4 &&
                  layout.elements[0].byte_offset == 0 &&
                  layout.elements[2].byte_offset == 16 &&
                  layout.elements[5].format == 0 &&
                  layout.elements[5].byte_offset == 0,
              "missing v0 materializes while missing v1-v15 omit");
    memset(output, 0xcc, sizeof(output));
    qxp_check(QxpD3d9GatherProgramVertex(
                  &prepared, views, &layout, 0,
                  output, sizeof(output)),
              "missing input vertex gathers");
    for (index = 0; index < 16; ++index)
        qxp_check(output[index] == 0,
                  "missing v0 is FLOAT4 raw zero");
    qxp_check(!memcmp(output + 16, source, sizeof(source)),
              "matched input follows materialized v0");
    qxp_check(qxp_make_prepared(
                  &prepared, &element, 1, 0x20UL,
                  usages, usage_indices, views) &&
                  QxpD3d9BuildProgramVertexLayout(
                      &prepared, views, &layout),
              "all-current-zero input layout builds");
    qxp_check(layout.used_inputs == 0x20UL &&
                  layout.input_mask == 0 &&
                  layout.vertex_stride == 0,
              "undefined v1-v15 need no vertex record");
    qxp_check(QxpD3d9GatherProgramVertex(
                  &prepared, views, &layout, 0, NULL, 0),
              "zero-stride all-current-zero gather touches no pointer");
}

static void qxp_test_bounds_and_overflow(void)
{
    QXP_VERTEX_ELEMENT9 element;
    QXP_VERTEX_STREAM_VIEW views[16];
    QXP_D3D9_DRAW_PREPARED prepared;
    QXP_D3D9_PROGRAM_VERTEX_LAYOUT layout;
    BYTE source[16];
    BYTE output[16];
    DWORD usages[16];
    DWORD usage_indices[16];
    QXP_VERTEX_STREAM_VIEW hostile_views[16];
    DWORD index;

    memset(&element, 0, sizeof(element));
    for (index = 0; index < sizeof(source); ++index)
        source[index] = (BYTE)(0x20 + index);
    memset(usages, 0, sizeof(usages));
    memset(usage_indices, 0, sizeof(usage_indices));
    qxp_zero_views(views);
    element.stream = 0;
    element.type = QXP_VERTEX_DECLTYPE9_FLOAT4;
    element.method = QXP_VERTEX_DECLMETHOD9_DEFAULT;
    element.usage = QXP_VERTEX_DECLUSAGE9_POSITION;
    views[0].base = source; views[0].bytes = sizeof(source);
    views[0].stride = sizeof(source); views[0].wraps = FALSE;
    usages[0] = QXP_VERTEX_DECLUSAGE9_POSITION;
    qxp_check(qxp_make_prepared(
                  &prepared, &element, 1, 1,
                  usages, usage_indices, views) &&
                  QxpD3d9BuildProgramVertexLayout(
                      &prepared, views, &layout),
              "bounds fixture builds");
    memset(output, 0x7d, sizeof(output));
    qxp_check(!QxpD3d9GatherProgramVertex(
                  &prepared, views, &layout, 0, output, 15),
              "destination bound rejects short record");
    qxp_check(output[0] == 0x7d,
              "destination remains unchanged after preflight failure");
    qxp_check(!QxpD3d9GatherProgramVertex(
                  &prepared, views, &layout, 1,
                  output, sizeof(output)),
              "source end bound rejects next vertex");
    qxp_check(!QxpD3d9GatherProgramVertex(
                  &prepared, views, &layout, 0xffffffffUL,
                  output, sizeof(output)),
              "hostile vertex multiply and source add reject");

    memcpy(hostile_views, views, sizeof(hostile_views));
    hostile_views[0].bytes = 15;
    qxp_check(!QxpD3d9BuildProgramVertexLayout(
                  &prepared, hostile_views, &layout),
              "view shorter than required span rejects");
    memcpy(hostile_views, views, sizeof(hostile_views));
    hostile_views[0].stride = 15;
    qxp_check(!QxpD3d9BuildProgramVertexLayout(
                  &prepared, hostile_views, &layout),
              "view stride mismatch and short span reject");
    memcpy(hostile_views, views, sizeof(hostile_views));
    hostile_views[0].base = NULL;
    qxp_check(!QxpD3d9BuildProgramVertexLayout(
                  &prepared, hostile_views, &layout),
              "null required view base rejects");
    memcpy(hostile_views, views, sizeof(hostile_views));
    hostile_views[0].wraps = (BOOL)2;
    qxp_check(!QxpD3d9BuildProgramVertexLayout(
                  &prepared, hostile_views, &layout),
              "noncanonical view wrap flag rejects");
    element.offset = 12;
    views[0].stride = 28;
    views[0].wraps = TRUE;
    qxp_check(qxp_make_prepared(
                  &prepared, &element, 1, 1,
                  usages, usage_indices, views) &&
                  QxpD3d9BuildProgramVertexLayout(
                      &prepared, views, &layout) &&
                  QxpD3d9GatherProgramVertex(
                      &prepared, views, &layout, 0,
                      output, sizeof(output)),
              "wrapped view gathers crossing element");
    qxp_check(!memcmp(output, source + 12, 4) &&
                  !memcmp(output + 4, source, 12),
              "wrapped source offset copies both segments");
    prepared.raw_state.streams[0].offset = 0xfffffff1UL;
    prepared.raw_state.streams[0].source2 = TRUE;
    qxp_check(!QxpD3d9BuildProgramVertexLayout(
                  &prepared, views, &layout),
              "prepared stream offset plus span overflow rejects");
}

static void qxp_test_unsupported_and_hostile(void)
{
    QXP_VERTEX_ELEMENT9 element;
    QXP_VERTEX_ELEMENT9 method_elements[2];
    QXP_VERTEX_STREAM_VIEW views[16];
    QXP_D3D9_DRAW_PREPARED prepared;
    QXP_D3D9_DRAW_PREPARED hostile;
    QXP_D3D9_PROGRAM_VERTEX_LAYOUT layout;
    QXP_D3D9_PROGRAM_VERTEX_LAYOUT changed;
    BYTE source[16];
    BYTE output[16];
    DWORD usages[16];
    DWORD usage_indices[16];

    memset(&element, 0, sizeof(element));
    memset(source, 0x19, sizeof(source));
    memset(usages, 0, sizeof(usages));
    memset(usage_indices, 0, sizeof(usage_indices));
    qxp_zero_views(views);
    element.stream = 0;
    element.type = QXP_VERTEX_DECLTYPE9_FLOAT4;
    element.method = QXP_VERTEX_DECLMETHOD9_DEFAULT;
    element.usage = QXP_VERTEX_DECLUSAGE9_POSITION;
    views[0].base = source; views[0].bytes = sizeof(source);
    views[0].stride = sizeof(source); views[0].wraps = FALSE;
    usages[0] = QXP_VERTEX_DECLUSAGE9_POSITION;
    qxp_check(qxp_make_prepared(
                  &prepared, &element, 1, 1,
                  usages, usage_indices, views) &&
                  QxpD3d9BuildProgramVertexLayout(
                      &prepared, views, &layout),
              "hostile fixture builds");

    hostile = prepared;
    hostile.vertex_link.inputs[0].source_offset = 4;
    qxp_check(!QxpD3d9BuildProgramVertexLayout(
                  &hostile, views, &changed),
              "hostile link mutation rejects");
    hostile = prepared;
    hostile.declaration.elements[0].byte_size = 4;
    qxp_check(!QxpD3d9BuildProgramVertexLayout(
                  &hostile, views, &changed),
              "hostile parsed declaration mutation rejects");
    hostile = prepared;
    hostile.undefined_vertex_input_defaults[3][2] = 1;
    qxp_check(!QxpD3d9BuildProgramVertexLayout(
                  &hostile, views, &changed),
              "hostile undefined default mutation rejects");

    changed = layout; changed.elements[0].byte_offset = 1;
    qxp_check(!QxpD3d9GatherProgramVertex(
                  &prepared, views, &changed, 0,
                  output, sizeof(output)),
              "hostile layout offset mutation rejects");
    changed = layout; changed.elements[0].format = 2;
    qxp_check(!QxpD3d9GatherProgramVertex(
                  &prepared, views, &changed, 0,
                  output, sizeof(output)),
              "hostile layout format mutation rejects");
    changed = layout; changed.vertex_stride = 15;
    qxp_check(!QxpD3d9GatherProgramVertex(
                  &prepared, views, &changed, 0,
                  output, sizeof(output)),
              "hostile layout stride mutation rejects");
    changed = layout; changed.valid = FALSE;
    qxp_check(!QxpD3d9GatherProgramVertex(
                  &prepared, views, &changed, 0,
                  output, sizeof(output)),
              "hostile layout validity mutation rejects");

    method_elements[0] = element;
    method_elements[1] = element;
    method_elements[1].method = QXP_VERTEX_DECLMETHOD9_PARTIALU;
    method_elements[1].usage_index = 1;
    qxp_check(qxp_make_prepared(
                  &prepared, method_elements, 2, 1,
                  usages, usage_indices, views),
              "nondefault declaration method parses upstream");
    qxp_check(!QxpD3d9BuildProgramVertexLayout(
                  &prepared, views, &changed),
              "nondefault declaration method fails native gather closed");

    element.type = QXP_VERTEX_DECLTYPE9_UBYTE4N;
    views[0].stride = 4;
    views[0].bytes = sizeof(source);
    qxp_check(qxp_make_prepared(
                  &prepared, &element, 1, 1,
                  usages, usage_indices, views),
              "normalized declaration parses upstream");
    qxp_check(!QxpD3d9BuildProgramVertexLayout(
                  &prepared, views, &changed),
              "unadvertised normalized declaration type rejects");
}

static void qxp_test_output_masks(void)
{
    QXP_VERTEX_ELEMENT9 element;
    QXP_VERTEX_STREAM_VIEW views[16];
    QXP_D3D9_DRAW_PREPARED prepared;
    QXP_D3D9_PROGRAM_VERTEX_LAYOUT layout;
    BYTE source[16];
    DWORD usages[16];
    DWORD usage_indices[16];
    DWORD expected;

    memset(&element, 0, sizeof(element));
    memset(source, 0, sizeof(source));
    memset(usages, 0, sizeof(usages));
    memset(usage_indices, 0, sizeof(usage_indices));
    qxp_zero_views(views);
    element.stream = 0;
    element.type = QXP_VERTEX_DECLTYPE9_FLOAT4;
    element.method = QXP_VERTEX_DECLMETHOD9_DEFAULT;
    element.usage = QXP_VERTEX_DECLUSAGE9_POSITION;
    views[0].base = source; views[0].bytes = sizeof(source);
    views[0].stride = sizeof(source); views[0].wraps = FALSE;
    usages[0] = QXP_VERTEX_DECLUSAGE9_POSITION;
    qxp_check(qxp_make_prepared(
                  &prepared, &element, 1, 1,
                  usages, usage_indices, views),
              "output-mask fixture parses");
    prepared.vertex_metadata.writes_fog = TRUE;
    prepared.vertex_metadata.fog_component_mask = 1;
    prepared.vertex_metadata.writes_point_size = TRUE;
    prepared.vertex_metadata.point_size_component_mask = 1;
    prepared.vertex_metadata.attribute_output_mask = 3;
    prepared.vertex_metadata.attribute_output_components[0] = 15;
    prepared.vertex_metadata.attribute_output_components[1] = 7;
    prepared.vertex_metadata.texcoord_output_mask = 7;
    prepared.vertex_metadata.texcoord_output_components[0] = 1;
    prepared.vertex_metadata.texcoord_output_components[1] = 3;
    prepared.vertex_metadata.texcoord_output_components[2] = 15;
    expected = QXP_D3D_VS_OUTPUT_POSITION | QXP_D3D_VS_OUTPUT_FOG |
               QXP_D3D_VS_OUTPUT_POINT_SIZE | QXP_D3D_VS_OUTPUT_COLOR0 |
               QXP_D3D_VS_OUTPUT_COLOR1 | QXP_D3D_VS_OUTPUT_TEXCOORD0 |
               (QXP_D3D_VS_OUTPUT_TEXCOORD0 << 1) |
               (QXP_D3D_VS_OUTPUT_TEXCOORD0 << 2);
    qxp_check(QxpD3d9BuildProgramVertexLayout(
                  &prepared, views, &layout) &&
                  layout.written_outputs == expected,
              "prepared output metadata maps to transport output mask");
    prepared.vertex_metadata.texcoord_output_mask = 0x80UL;
    memset(prepared.vertex_metadata.texcoord_output_components, 0,
           sizeof(prepared.vertex_metadata.texcoord_output_components));
    prepared.vertex_metadata.texcoord_output_components[7] = 15;
    expected = QXP_D3D_VS_OUTPUT_POSITION | QXP_D3D_VS_OUTPUT_FOG |
               QXP_D3D_VS_OUTPUT_POINT_SIZE | QXP_D3D_VS_OUTPUT_COLOR0 |
               QXP_D3D_VS_OUTPUT_COLOR1 |
               (QXP_D3D_VS_OUTPUT_TEXCOORD0 << 7);
    qxp_check(QxpD3d9BuildProgramVertexLayout(
                  &prepared, views, &layout) &&
                  layout.written_outputs == expected,
              "sparse oT7 metadata maps without invented outputs");
}

int main(void)
{
    qxp_test_all_types_and_color();
    qxp_test_multistream_register_order();
    qxp_test_missing_inputs();
    qxp_test_bounds_and_overflow();
    qxp_test_unsupported_and_hostile();
    qxp_test_output_masks();
    if (qxp_failures) {
        fprintf(stderr, "%d qxp_d3d9_draw_execute test(s) failed\n",
                qxp_failures);
        return 1;
    }
    puts("qxp_d3d9_draw_execute tests passed");
    return 0;
}
