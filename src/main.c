#include <efi.h>
#include <efilib.h>
#include <efiprot.h>
#include <eficonex.h>

#define INPUT_BUFFER_SIZE 256
#define MAX_COMMAND_ARGS 16

#define MIIMG_FORMAT_BGRA_RLE 1

#define MIGIF_FILE_HEADER_SIZE 49
#define MIGIF_BLOCK_HEADER_SIZE 8
#define MIGIF_FRAME_HEADER_SIZE 28
#define MIGIF_FULL_PAYLOAD_HEADER_SIZE 16
#define MIGIF_DELTA_PAYLOAD_HEADER_SIZE 4
#define MIGIF_DELTA_RECT_HEADER_SIZE 20
#define MIGIF_TRANSFORM_PAYLOAD_SIZE 28

#define MIGIF_FLAG_LOOP 0x01
#define MIGIF_FLAG_HAS_PLTE 0x02

#define MIGIF_COLOR_RGBA8888 0

#define MIGIF_FRAME_FULL 1
#define MIGIF_FRAME_DELTA 2
#define MIGIF_FRAME_TRANSFORM 3

#define MIGIF_ENCODING_RAW_RGBA8888 0
#define MIGIF_ENCODING_RLE_RGBA8888 1
#define MIGIF_ENCODING_RAW_INDEX8 2
#define MIGIF_ENCODING_RLE_INDEX8 3

#define MIGIF_TF_POS_X (1U << 0)
#define MIGIF_TF_POS_Y (1U << 1)
#define MIGIF_TF_ALPHA (1U << 2)
#define MIGIF_TF_SCALE_X (1U << 3)
#define MIGIF_TF_SCALE_Y (1U << 4)
#define MIGIF_TF_ROTATION (1U << 5)

#define FIXED_POINT_ONE 65536
#define INVALID_FRAME_INDEX 0xffffffffU
#define PLAYBACK_TICK_100NS 50000ULL
#define SEEK_STEP_100NS 5000000ULL
#define SPACE_DEBOUNCE_TICKS 40
#define SPLASH_STEP_US 50000U
#define SPLASH_STEP_100NS 500000ULL
#define SPLASH_FADE_STEPS 40U
#define SPLASH_HOLD_STEPS 10U

static EFI_HANDLE gImageHandle = NULL;

static CHAR16 *MIOS_VERSION = L"FakeMIOS v0.0.1";
static CHAR16 *MIOS_EMBLEM = L"  _____      _         __  __ ___ ___  ____\r\n |  ___|_ _ | | __ ___|  \\/  |_ _/ _ \\/ ___|\r\n | |_ / _` || |/ // _ \\ |\\/| || | | | \\___ \\\r\n |  _| (_| ||   <|  __/ |  | || | |_| |___) |\r\n |_|  \\__,_||_|\\_\\____|_|  |_|___\\___/|____/\r\n";
static CHAR16 *MIOS_SPLASH_ASSET = L"logo";

typedef struct {
    CHAR16 *name;
    UINTN argc;
    CHAR16 *argv[MAX_COMMAND_ARGS];
} ParsedCommand;

typedef struct {
    UINT32 width;
    UINT32 height;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *pixels;
} DecodedImage;

typedef struct {
    EFI_INPUT_KEY key;
    EFI_KEY_STATE key_state;
    BOOLEAN has_extended_state;
} PlaybackInput;

typedef struct {
    UINT32 color_count;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *colors;
} MigifPalette;

typedef struct {
    UINT32 x;
    UINT32 y;
    UINT32 width;
    UINT32 height;
} MigifDirtyRect;

typedef struct {
    UINT16 type;
    UINT16 flags;
    UINT32 duration_num;
    UINT32 duration_den;
    UINT32 payload_size;
    UINT32 reserved0;
    UINT32 reserved1;
    UINT32 full_reset_index;
    UINT64 duration_100ns;
    const UINT8 *payload;
} MigifFrame;

typedef struct {
    const UINT8 *data;
    UINTN data_size;
    UINT8 version_major;
    UINT8 version_minor;
    UINT8 flags;
    UINT32 canvas_width;
    UINT32 canvas_height;
    UINT32 frame_count;
    UINT32 fps_num;
    UINT32 fps_den;
    MigifPalette palette;
    MigifFrame *frames;
    UINT64 *frame_start_times;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL **frame_cache;
    MigifDirtyRect *frame_dirty_rects;
    MigifDirtyRect loop_dirty_rect;
    UINTN canvas_pixel_count;
    UINT64 total_duration_100ns;
} MigifFile;

typedef struct {
    INT32 pos_x;
    INT32 pos_y;
    UINT32 alpha;
    INT32 scale_x;
    INT32 scale_y;
    INT32 rotation_deg;
} MigifTransform;

typedef struct {
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *canvas;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *screen_buffer;
    UINTN canvas_pixel_count;
    UINT32 current_frame;
    UINT64 frame_elapsed_100ns;
    MigifTransform transform;
    BOOLEAN paused;
    BOOLEAN reached_end;
    UINT64 space_debounce_until;
    BOOLEAN has_rendered_frame;
    BOOLEAN force_full_redraw;
    UINT32 last_rendered_frame;
} MigifPlayerState;

static EFI_STATUS render_centered_faded_pixels(
    EFI_SYSTEM_TABLE *SystemTable,
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
    const EFI_GRAPHICS_OUTPUT_BLT_PIXEL *pixels,
    UINT32 width,
    UINT32 height,
    UINT8 global_alpha
);

static void print_prompt(void) {
    Print(L"> ");
}

static INTN str_cmp(const CHAR16 *a, const CHAR16 *b) {
    while (*a != L'\0' && *b != L'\0') {
        if (*a != *b) {
            return *a - *b;
        }
        a++;
        b++;
    }

    return *a - *b;
}

static UINTN str_len(const CHAR16 *text) {
    UINTN len = 0;

    while (text[len] != L'\0') {
        len++;
    }

    return len;
}

static BOOLEAN is_space(CHAR16 ch) {
    return ch == L' ' || ch == L'\t';
}

static BOOLEAN is_command(const ParsedCommand *command, const CHAR16 *name) {
    return command->name != NULL && str_cmp(command->name, name) == 0;
}

static void parse_command(CHAR16 *input, ParsedCommand *command) {
    UINTN i = 0;

    command->name = NULL;
    command->argc = 0;
    for (UINTN arg = 0; arg < MAX_COMMAND_ARGS; arg++) {
        command->argv[arg] = NULL;
    }

    while (input[i] != L'\0') {
        while (is_space(input[i])) {
            input[i++] = L'\0';
        }

        if (input[i] == L'\0') {
            break;
        }

        CHAR16 *token;
        if (input[i] == L'"') {
            i++;
            token = &input[i];

            while (input[i] != L'\0' && input[i] != L'"') {
                i++;
            }

            if (input[i] == L'"') {
                input[i++] = L'\0';
            }
        } else {
            token = &input[i];

            while (input[i] != L'\0' && !is_space(input[i])) {
                i++;
            }

            if (input[i] != L'\0') {
                input[i++] = L'\0';
            }
        }

        if (command->name == NULL) {
            command->name = token;
            continue;
        }

        if (command->argc < MAX_COMMAND_ARGS) {
            command->argv[command->argc++] = token;
        }
    }
}

static void clear_buffer(CHAR16 *buf, UINTN size) {
    for (UINTN i = 0; i < size; i++) {
        buf[i] = L'\0';
    }
}

static void zero_bytes(VOID *buffer, UINTN size) {
    UINT8 *bytes = (UINT8 *)buffer;

    for (UINTN i = 0; i < size; i++) {
        bytes[i] = 0;
    }
}

static UINT16 read_u16le(const UINT8 *data, UINTN offset) {
    return (UINT16)data[offset]
        | ((UINT16)data[offset + 1] << 8);
}

static UINT32 read_u32le(const UINT8 *data, UINTN offset) {
    return (UINT32)data[offset]
        | ((UINT32)data[offset + 1] << 8)
        | ((UINT32)data[offset + 2] << 16)
        | ((UINT32)data[offset + 3] << 24);
}

static INT32 read_s32le(const UINT8 *data, UINTN offset) {
    return (INT32)read_u32le(data, offset);
}

static EFI_GRAPHICS_OUTPUT_BLT_PIXEL rgba_to_blt(UINT8 red, UINT8 green, UINT8 blue, UINT8 alpha) {
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL pixel;
    pixel.Red = red;
    pixel.Green = green;
    pixel.Blue = blue;
    pixel.Reserved = alpha;
    return pixel;
}

static void fill_pixel_buffer(
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *pixels,
    UINTN count,
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL color
) {
    for (UINTN i = 0; i < count; i++) {
        pixels[i] = color;
    }
}

static EFI_STATUS get_pixel_count(UINT32 width, UINT32 height, UINTN *out_count) {
    if (width == 0 || height == 0) {
        return EFI_COMPROMISED_DATA;
    }

    UINT64 count = (UINT64)width * (UINT64)height;
    if (count > (UINT64)(~(UINTN)0)) {
        return EFI_COMPROMISED_DATA;
    }

    *out_count = (UINTN)count;
    return EFI_SUCCESS;
}

static EFI_INPUT_KEY wait_key(EFI_SYSTEM_TABLE *SystemTable) {
    EFI_INPUT_KEY key;
    UINTN index = 0;

    zero_bytes(&key, sizeof(key));
    uefi_call_wrapper(
        SystemTable->BootServices->WaitForEvent,
        3,
        1,
        &SystemTable->ConIn->WaitForKey,
        &index
    );
    uefi_call_wrapper(SystemTable->ConIn->ReadKeyStroke, 2, SystemTable->ConIn, &key);
    return key;
}

static void read_line(EFI_SYSTEM_TABLE *SystemTable, CHAR16 *buffer, UINTN max_len) {
    UINTN pos = 0;
    clear_buffer(buffer, max_len);

    while (1) {
        EFI_INPUT_KEY key = wait_key(SystemTable);

        if (key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
            Print(L"\r\n");
            buffer[pos] = L'\0';
            return;
        }

        if (key.UnicodeChar == CHAR_BACKSPACE) {
            if (pos > 0) {
                pos--;
                buffer[pos] = L'\0';
                Print(L"\b \b");
            }
            continue;
        }

        if (key.UnicodeChar >= 32 && key.UnicodeChar <= 126) {
            if (pos < max_len - 1) {
                buffer[pos++] = key.UnicodeChar;
                CHAR16 tmp[2];
                tmp[0] = key.UnicodeChar;
                tmp[1] = L'\0';
                Print(tmp);
            }
        }
    }
}

static void PrintLn(CHAR16 *input) {
    Print(input);
    Print(L"\r\n");
}

static EFI_STATUS allocate_pool(
    EFI_SYSTEM_TABLE *SystemTable,
    UINTN size,
    VOID **buffer
) {
    return uefi_call_wrapper(
        SystemTable->BootServices->AllocatePool,
        3,
        EfiLoaderData,
        size,
        buffer
    );
}

static void free_pool(EFI_SYSTEM_TABLE *SystemTable, VOID *buffer) {
    if (buffer != NULL) {
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, buffer);
    }
}

static EFI_STATUS allocate_pixel_buffer(
    EFI_SYSTEM_TABLE *SystemTable,
    UINT32 width,
    UINT32 height,
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL **out_pixels
) {
    UINTN pixel_count = 0;
    EFI_STATUS status = get_pixel_count(width, height, &pixel_count);
    if (EFI_ERROR(status)) {
        return status;
    }

    if (pixel_count > (~(UINTN)0) / sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL)) {
        return EFI_COMPROMISED_DATA;
    }

    return allocate_pool(
        SystemTable,
        pixel_count * sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL),
        (VOID **)out_pixels
    );
}

static void show_terminal_home(EFI_SYSTEM_TABLE *SystemTable) {
    uefi_call_wrapper(SystemTable->ConOut->ClearScreen, 1, SystemTable->ConOut);
    PrintLn(MIOS_EMBLEM);
    PrintLn(L"");
    PrintLn(MIOS_VERSION);
    PrintLn(L"Terminal");
    PrintLn(L"Type 'help' for commands.\r\n");
}

static EFI_STATUS get_gop(
    EFI_SYSTEM_TABLE *SystemTable,
    EFI_GRAPHICS_OUTPUT_PROTOCOL **gop
) {
    return uefi_call_wrapper(
        SystemTable->BootServices->LocateProtocol,
        3,
        &GraphicsOutputProtocol,
        NULL,
        (VOID **)gop
    );
}

static EFI_STATUS get_text_input_ex(
    EFI_SYSTEM_TABLE *SystemTable,
    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL **input_ex
) {
    return uefi_call_wrapper(
        SystemTable->BootServices->LocateProtocol,
        3,
        &SimpleTextInputExProtocol,
        NULL,
        (VOID **)input_ex
    );
}

static EFI_STATUS fill_rect(
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL color,
    UINTN x,
    UINTN y,
    UINTN width,
    UINTN height
) {
    return uefi_call_wrapper(
        gop->Blt,
        10,
        gop,
        &color,
        EfiBltVideoFill,
        0,
        0,
        x,
        y,
        width,
        height,
        0
    );
}

static EFI_STATUS fill_screen(
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL color
) {
    return fill_rect(
        gop,
        color,
        0,
        0,
        gop->Mode->Info->HorizontalResolution,
        gop->Mode->Info->VerticalResolution
    );
}

static EFI_STATUS draw_blt_image(
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *pixels,
    UINTN width,
    UINTN height,
    UINTN x,
    UINTN y
) {
    return uefi_call_wrapper(
        gop->Blt,
        10,
        gop,
        pixels,
        EfiBltBufferToVideo,
        0,
        0,
        x,
        y,
        width,
        height,
        width * sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL)
    );
}

static EFI_STATUS draw_blt_subimage(
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *pixels,
    UINTN buffer_width,
    UINTN src_x,
    UINTN src_y,
    UINTN width,
    UINTN height,
    UINTN dst_x,
    UINTN dst_y
) {
    if (width == 0 || height == 0) {
        return EFI_SUCCESS;
    }

    return uefi_call_wrapper(
        gop->Blt,
        10,
        gop,
        pixels,
        EfiBltBufferToVideo,
        src_x,
        src_y,
        dst_x,
        dst_y,
        width,
        height,
        buffer_width * sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL)
    );
}

static EFI_STATUS draw_image_centered(
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *pixels,
    UINTN width,
    UINTN height
) {
    UINTN screen_width = gop->Mode->Info->HorizontalResolution;
    UINTN screen_height = gop->Mode->Info->VerticalResolution;

    if (width > screen_width || height > screen_height) {
        return EFI_UNSUPPORTED;
    }

    return draw_blt_image(
        gop,
        pixels,
        width,
        height,
        (screen_width - width) / 2,
        (screen_height - height) / 2
    );
}

static void wait_for_exit_key(EFI_SYSTEM_TABLE *SystemTable) {
    while (1) {
        EFI_INPUT_KEY key = wait_key(SystemTable);
        if (key.UnicodeChar == L'q' || key.UnicodeChar == L'Q') {
            return;
        }
    }
}

static EFI_STATUS rle_unpack_bgra(
    EFI_SYSTEM_TABLE *SystemTable,
    const UINT8 *data,
    UINTN data_size,
    UINT32 width,
    UINT32 height,
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL **out_pixels
) {
    UINTN pixel_count = 0;
    EFI_STATUS status = get_pixel_count(width, height, &pixel_count);
    if (EFI_ERROR(status)) {
        return status;
    }

    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *pixels = NULL;
    status = allocate_pixel_buffer(SystemTable, width, height, &pixels);
    if (EFI_ERROR(status)) {
        return status;
    }

    UINTN src = 0;
    UINTN dst = 0;

    while (src < data_size && dst < pixel_count) {
        UINT8 ctrl = data[src++];

        if (ctrl < 128) {
            UINTN run_len = (UINTN)ctrl + 1;
            UINTN byte_len = run_len * 4;

            if (src + byte_len > data_size || dst + run_len > pixel_count) {
                free_pool(SystemTable, pixels);
                return EFI_COMPROMISED_DATA;
            }

            for (UINTN i = 0; i < run_len; i++) {
                UINTN offset = src + (i * 4);
                pixels[dst + i].Blue = data[offset + 0];
                pixels[dst + i].Green = data[offset + 1];
                pixels[dst + i].Red = data[offset + 2];
                pixels[dst + i].Reserved = data[offset + 3];
            }

            src += byte_len;
            dst += run_len;
            continue;
        }

        UINTN run_len = (UINTN)(ctrl - 128) + 1;
        if (src + 4 > data_size || dst + run_len > pixel_count) {
            free_pool(SystemTable, pixels);
            return EFI_COMPROMISED_DATA;
        }

        EFI_GRAPHICS_OUTPUT_BLT_PIXEL pixel;
        pixel.Blue = data[src + 0];
        pixel.Green = data[src + 1];
        pixel.Red = data[src + 2];
        pixel.Reserved = data[src + 3];
        src += 4;

        for (UINTN i = 0; i < run_len; i++) {
            pixels[dst++] = pixel;
        }
    }

    if (src != data_size || dst != pixel_count) {
        free_pool(SystemTable, pixels);
        return EFI_COMPROMISED_DATA;
    }

    *out_pixels = pixels;
    return EFI_SUCCESS;
}

static EFI_STATUS decode_miimg(
    EFI_SYSTEM_TABLE *SystemTable,
    const UINT8 *data,
    UINTN data_size,
    DecodedImage *image
) {
    if (data_size < 20) {
        return EFI_COMPROMISED_DATA;
    }

    if (data[0] != 'M' || data[1] != 'I' || data[2] != 'I' || data[3] != '0') {
        return EFI_COMPROMISED_DATA;
    }

    UINT32 width = read_u32le(data, 4);
    UINT32 height = read_u32le(data, 8);
    UINT32 format = read_u32le(data, 12);
    UINT32 packed_size = read_u32le(data, 16);

    if (width == 0 || height == 0 || format != MIIMG_FORMAT_BGRA_RLE) {
        return EFI_UNSUPPORTED;
    }

    if ((UINTN)packed_size > data_size - 20) {
        return EFI_COMPROMISED_DATA;
    }

    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *pixels = NULL;
    EFI_STATUS status = rle_unpack_bgra(
        SystemTable,
        data + 20,
        (UINTN)packed_size,
        width,
        height,
        &pixels
    );
    if (EFI_ERROR(status)) {
        return status;
    }

    image->width = width;
    image->height = height;
    image->pixels = pixels;
    return EFI_SUCCESS;
}

static void free_decoded_image(EFI_SYSTEM_TABLE *SystemTable, DecodedImage *image) {
    free_pool(SystemTable, image->pixels);
    image->width = 0;
    image->height = 0;
    image->pixels = NULL;
}

static EFI_STATUS open_root_volume(
    EFI_SYSTEM_TABLE *SystemTable,
    EFI_FILE_HANDLE *root
) {
    EFI_LOADED_IMAGE *loaded_image = NULL;
    EFI_STATUS status = uefi_call_wrapper(
        SystemTable->BootServices->HandleProtocol,
        3,
        gImageHandle,
        &LoadedImageProtocol,
        (VOID **)&loaded_image
    );
    if (EFI_ERROR(status) || loaded_image == NULL) {
        return EFI_NOT_FOUND;
    }

    *root = LibOpenRoot(loaded_image->DeviceHandle);
    return *root == NULL ? EFI_NOT_FOUND : EFI_SUCCESS;
}

static EFI_STATUS reload_grubx64(EFI_SYSTEM_TABLE *SystemTable) {
    EFI_LOADED_IMAGE *loaded_image = NULL;
    EFI_STATUS status = uefi_call_wrapper(
        SystemTable->BootServices->HandleProtocol,
        3,
        gImageHandle,
        &LoadedImageProtocol,
        (VOID **)&loaded_image
    );
    if (EFI_ERROR(status) || loaded_image == NULL) {
        return EFI_NOT_FOUND;
    }

    EFI_DEVICE_PATH *boot_path = FileDevicePath(loaded_image->DeviceHandle, L"\\EFI\\BOOT\\grubx64.efi");
    if (boot_path == NULL) {
        return EFI_OUT_OF_RESOURCES;
    }

    EFI_HANDLE boot_handle = NULL;
    status = uefi_call_wrapper(
        SystemTable->BootServices->LoadImage,
        6,
        FALSE,
        gImageHandle,
        boot_path,
        NULL,
        0,
        &boot_handle
    );
    free_pool(SystemTable, boot_path);
    if (EFI_ERROR(status)) {
        return status;
    }

    UINTN exit_data_size = 0;
    CHAR16 *exit_data = NULL;
    status = uefi_call_wrapper(
        SystemTable->BootServices->StartImage,
        3,
        boot_handle,
        &exit_data_size,
        &exit_data
    );
    if (exit_data != NULL) {
        free_pool(SystemTable, exit_data);
    }

    if (EFI_ERROR(status)) {
        uefi_call_wrapper(SystemTable->BootServices->UnloadImage, 1, boot_handle);
        return status;
    }

    uefi_call_wrapper(SystemTable->BootServices->UnloadImage, 1, boot_handle);
    return EFI_SUCCESS;
}

static EFI_STATUS build_asset_path(
    EFI_SYSTEM_TABLE *SystemTable,
    const CHAR16 *asset_name,
    const CHAR16 *suffix,
    CHAR16 **out_path
) {
    const CHAR16 *prefix = L"\\EFI\\BOOT\\assets\\";
    UINTN prefix_len = str_len(prefix);
    UINTN name_len = str_len(asset_name);
    UINTN suffix_len = str_len(suffix);
    UINTN total_len = prefix_len + name_len + suffix_len + 1;
    CHAR16 *path = NULL;

    EFI_STATUS status = allocate_pool(SystemTable, total_len * sizeof(CHAR16), (VOID **)&path);
    if (EFI_ERROR(status)) {
        return status;
    }

    UINTN pos = 0;
    for (UINTN i = 0; i < prefix_len; i++) {
        path[pos++] = prefix[i];
    }
    for (UINTN i = 0; i < name_len; i++) {
        path[pos++] = asset_name[i];
    }
    for (UINTN i = 0; i < suffix_len; i++) {
        path[pos++] = suffix[i];
    }
    path[pos] = L'\0';

    *out_path = path;
    return EFI_SUCCESS;
}

static EFI_STATUS open_asset_file(
    EFI_SYSTEM_TABLE *SystemTable,
    const CHAR16 *asset_name,
    const CHAR16 *suffix,
    EFI_FILE_HANDLE *file
) {
    EFI_FILE_HANDLE root = NULL;
    CHAR16 *path = NULL;
    EFI_STATUS status = build_asset_path(SystemTable, asset_name, suffix, &path);
    if (EFI_ERROR(status)) {
        return status;
    }

    status = open_root_volume(SystemTable, &root);
    if (EFI_ERROR(status)) {
        free_pool(SystemTable, path);
        return status;
    }

    status = uefi_call_wrapper(
        root->Open,
        5,
        root,
        file,
        path,
        EFI_FILE_MODE_READ,
        0
    );

    uefi_call_wrapper(root->Close, 1, root);
    free_pool(SystemTable, path);
    return status;
}

static EFI_STATUS read_entire_file(
    EFI_SYSTEM_TABLE *SystemTable,
    EFI_FILE_HANDLE file,
    UINT8 **out_data,
    UINTN *out_size
) {
    EFI_FILE_INFO *file_info = LibFileInfo(file);
    if (file_info == NULL) {
        return EFI_DEVICE_ERROR;
    }

    UINTN size = (UINTN)file_info->FileSize;
    free_pool(SystemTable, file_info);

    UINT8 *data = NULL;
    EFI_STATUS status = allocate_pool(SystemTable, size, (VOID **)&data);
    if (EFI_ERROR(status)) {
        return status;
    }

    UINTN read_size = size;
    status = uefi_call_wrapper(file->Read, 3, file, &read_size, data);
    if (EFI_ERROR(status) || read_size != size) {
        free_pool(SystemTable, data);
        return EFI_DEVICE_ERROR;
    }

    *out_data = data;
    *out_size = size;
    return EFI_SUCCESS;
}

static void close_file(EFI_FILE_HANDLE file) {
    if (file != NULL) {
        uefi_call_wrapper(file->Close, 1, file);
    }
}

static void migif_transform_identity(MigifTransform *transform) {
    transform->pos_x = 0;
    transform->pos_y = 0;
    transform->alpha = 255;
    transform->scale_x = FIXED_POINT_ONE;
    transform->scale_y = FIXED_POINT_ONE;
    transform->rotation_deg = 0;
}

static void free_migif_palette(EFI_SYSTEM_TABLE *SystemTable, MigifPalette *palette) {
    free_pool(SystemTable, palette->colors);
    palette->colors = NULL;
    palette->color_count = 0;
}

static void free_migif_frame_cache(EFI_SYSTEM_TABLE *SystemTable, MigifFile *migif) {
    if (migif->frame_cache != NULL) {
        for (UINT32 i = 0; i < migif->frame_count; i++) {
            free_pool(SystemTable, migif->frame_cache[i]);
        }
    }

    free_pool(SystemTable, migif->frame_cache);
    free_pool(SystemTable, migif->frame_dirty_rects);
    migif->frame_cache = NULL;
    migif->frame_dirty_rects = NULL;
    migif->loop_dirty_rect.x = 0;
    migif->loop_dirty_rect.y = 0;
    migif->loop_dirty_rect.width = 0;
    migif->loop_dirty_rect.height = 0;
}

static void free_migif_file(EFI_SYSTEM_TABLE *SystemTable, MigifFile *migif) {
    free_migif_frame_cache(SystemTable, migif);
    free_migif_palette(SystemTable, &migif->palette);
    free_pool(SystemTable, migif->frames);
    free_pool(SystemTable, migif->frame_start_times);
    free_pool(SystemTable, (VOID *)migif->data);

    migif->data = NULL;
    migif->data_size = 0;
    migif->frames = NULL;
    migif->frame_start_times = NULL;
    migif->frame_count = 0;
    migif->canvas_pixel_count = 0;
    migif->total_duration_100ns = 0;
}

static void free_migif_player(EFI_SYSTEM_TABLE *SystemTable, MigifPlayerState *player) {
    free_pool(SystemTable, player->canvas);
    free_pool(SystemTable, player->screen_buffer);
    player->canvas = NULL;
    player->screen_buffer = NULL;
    player->canvas_pixel_count = 0;
    player->current_frame = 0;
    player->frame_elapsed_100ns = 0;
    player->paused = TRUE;
    player->reached_end = FALSE;
    player->space_debounce_until = 0;
    player->has_rendered_frame = FALSE;
    player->force_full_redraw = TRUE;
    player->last_rendered_frame = 0;
    migif_transform_identity(&player->transform);
}

static EFI_STATUS migif_duration_to_100ns(UINT32 num, UINT32 den, UINT64 *out_duration) {
    if (den == 0) {
        return EFI_COMPROMISED_DATA;
    }

    UINT64 scaled = ((UINT64)num * 10000000ULL) + ((UINT64)den / 2ULL);
    *out_duration = scaled / (UINT64)den;
    return EFI_SUCCESS;
}

static EFI_STATUS validate_plte_block(
    EFI_SYSTEM_TABLE *SystemTable,
    const UINT8 *payload,
    UINTN payload_size,
    MigifPalette *palette
) {
    if (payload_size < 8) {
        return EFI_COMPROMISED_DATA;
    }

    UINT32 color_count = read_u32le(payload, 0);
    UINT8 color_format = payload[4];
    UINT64 expected_size = 8ULL + ((UINT64)color_count * 4ULL);

    if (color_format != MIGIF_COLOR_RGBA8888 || expected_size != (UINT64)payload_size) {
        return EFI_COMPROMISED_DATA;
    }

    free_migif_palette(SystemTable, palette);

    if (color_count == 0) {
        return EFI_SUCCESS;
    }

    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *colors = NULL;
    EFI_STATUS status = allocate_pool(
        SystemTable,
        (UINTN)color_count * sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL),
        (VOID **)&colors
    );
    if (EFI_ERROR(status)) {
        return status;
    }

    UINTN offset = 8;
    for (UINT32 i = 0; i < color_count; i++) {
        colors[i] = rgba_to_blt(
            payload[offset + 0],
            payload[offset + 1],
            payload[offset + 2],
            payload[offset + 3]
        );
        offset += 4;
    }

    palette->color_count = color_count;
    palette->colors = colors;
    return EFI_SUCCESS;
}

static EFI_STATUS decode_migif_pixels_into_buffer(
    const MigifPalette *palette,
    UINT16 encoding,
    const UINT8 *data,
    UINTN data_size,
    UINT32 width,
    UINT32 height,
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *out_pixels
) {
    UINTN pixel_count = 0;
    EFI_STATUS status = get_pixel_count(width, height, &pixel_count);
    if (EFI_ERROR(status)) {
        return status;
    }

    if (encoding == MIGIF_ENCODING_RAW_RGBA8888) {
        UINT64 expected_size = (UINT64)pixel_count * 4ULL;
        if (expected_size != (UINT64)data_size) {
            return EFI_COMPROMISED_DATA;
        }

        if (out_pixels != NULL) {
            for (UINTN i = 0; i < pixel_count; i++) {
                UINTN src = i * 4;
                out_pixels[i] = rgba_to_blt(
                    data[src + 0],
                    data[src + 1],
                    data[src + 2],
                    data[src + 3]
                );
            }
        }

        return EFI_SUCCESS;
    }

    if (encoding == MIGIF_ENCODING_RLE_RGBA8888) {
        UINTN src = 0;
        UINTN dst = 0;

        while (src < data_size && dst < pixel_count) {
            if (src + 6 > data_size) {
                return EFI_COMPROMISED_DATA;
            }

            UINTN run_length = (UINTN)read_u16le(data, src);
            src += 2;

            if (run_length == 0 || dst + run_length > pixel_count) {
                return EFI_COMPROMISED_DATA;
            }

            EFI_GRAPHICS_OUTPUT_BLT_PIXEL pixel = rgba_to_blt(
                data[src + 0],
                data[src + 1],
                data[src + 2],
                data[src + 3]
            );
            src += 4;

            if (out_pixels != NULL) {
                for (UINTN i = 0; i < run_length; i++) {
                    out_pixels[dst + i] = pixel;
                }
            }

            dst += run_length;
        }

        if (src != data_size || dst != pixel_count) {
            return EFI_COMPROMISED_DATA;
        }

        return EFI_SUCCESS;
    }

    if (encoding == MIGIF_ENCODING_RAW_INDEX8) {
        if (palette == NULL || palette->colors == NULL) {
            return EFI_COMPROMISED_DATA;
        }

        if ((UINT64)pixel_count != (UINT64)data_size) {
            return EFI_COMPROMISED_DATA;
        }

        for (UINTN i = 0; i < pixel_count; i++) {
            UINT8 index = data[i];
            if ((UINT32)index >= palette->color_count) {
                return EFI_COMPROMISED_DATA;
            }

            if (out_pixels != NULL) {
                out_pixels[i] = palette->colors[index];
            }
        }

        return EFI_SUCCESS;
    }

    if (encoding == MIGIF_ENCODING_RLE_INDEX8) {
        if (palette == NULL || palette->colors == NULL) {
            return EFI_COMPROMISED_DATA;
        }

        UINTN src = 0;
        UINTN dst = 0;

        while (src < data_size && dst < pixel_count) {
            if (src + 3 > data_size) {
                return EFI_COMPROMISED_DATA;
            }

            UINTN run_length = (UINTN)read_u16le(data, src);
            UINT8 index = data[src + 2];
            src += 3;

            if (run_length == 0 || dst + run_length > pixel_count || (UINT32)index >= palette->color_count) {
                return EFI_COMPROMISED_DATA;
            }

            if (out_pixels != NULL) {
                EFI_GRAPHICS_OUTPUT_BLT_PIXEL pixel = palette->colors[index];
                for (UINTN i = 0; i < run_length; i++) {
                    out_pixels[dst + i] = pixel;
                }
            }

            dst += run_length;
        }

        if (src != data_size || dst != pixel_count) {
            return EFI_COMPROMISED_DATA;
        }

        return EFI_SUCCESS;
    }

    return EFI_UNSUPPORTED;
}

static EFI_STATUS decode_migif_pixels(
    EFI_SYSTEM_TABLE *SystemTable,
    const MigifPalette *palette,
    UINT16 encoding,
    const UINT8 *data,
    UINTN data_size,
    UINT32 width,
    UINT32 height,
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL **out_pixels
) {
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *pixels = NULL;
    EFI_STATUS status = allocate_pixel_buffer(SystemTable, width, height, &pixels);
    if (EFI_ERROR(status)) {
        return status;
    }

    status = decode_migif_pixels_into_buffer(
        palette,
        encoding,
        data,
        data_size,
        width,
        height,
        pixels
    );
    if (EFI_ERROR(status)) {
        free_pool(SystemTable, pixels);
        return status;
    }

    *out_pixels = pixels;
    return EFI_SUCCESS;
}

static EFI_STATUS validate_migif_full_payload(const MigifFile *migif, const MigifFrame *frame) {
    if (frame->payload_size < MIGIF_FULL_PAYLOAD_HEADER_SIZE) {
        return EFI_COMPROMISED_DATA;
    }

    UINT16 encoding = read_u16le(frame->payload, 0);
    UINT32 width = read_u32le(frame->payload, 4);
    UINT32 height = read_u32le(frame->payload, 8);
    UINT32 data_size = read_u32le(frame->payload, 12);

    if (width == 0 || height == 0) {
        return EFI_COMPROMISED_DATA;
    }

    if (width > migif->canvas_width || height > migif->canvas_height) {
        return EFI_COMPROMISED_DATA;
    }

    if ((UINTN)data_size != frame->payload_size - MIGIF_FULL_PAYLOAD_HEADER_SIZE) {
        return EFI_COMPROMISED_DATA;
    }

    return decode_migif_pixels_into_buffer(
        &migif->palette,
        encoding,
        frame->payload + MIGIF_FULL_PAYLOAD_HEADER_SIZE,
        (UINTN)data_size,
        width,
        height,
        NULL
    );
}

static EFI_STATUS validate_migif_delta_payload(const MigifFile *migif, const MigifFrame *frame) {
    if (frame->payload_size < MIGIF_DELTA_PAYLOAD_HEADER_SIZE) {
        return EFI_COMPROMISED_DATA;
    }

    UINT16 encoding = read_u16le(frame->payload, 0);
    UINT16 rect_count = read_u16le(frame->payload, 2);
    UINTN offset = MIGIF_DELTA_PAYLOAD_HEADER_SIZE;

    for (UINT16 rect_index = 0; rect_index < rect_count; rect_index++) {
        if (offset + MIGIF_DELTA_RECT_HEADER_SIZE > frame->payload_size) {
            return EFI_COMPROMISED_DATA;
        }

        UINT32 x = read_u32le(frame->payload, offset + 0);
        UINT32 y = read_u32le(frame->payload, offset + 4);
        UINT32 width = read_u32le(frame->payload, offset + 8);
        UINT32 height = read_u32le(frame->payload, offset + 12);
        UINT32 data_size = read_u32le(frame->payload, offset + 16);
        offset += MIGIF_DELTA_RECT_HEADER_SIZE;

        if (width == 0 || height == 0) {
            return EFI_COMPROMISED_DATA;
        }

        if (x > migif->canvas_width || y > migif->canvas_height) {
            return EFI_COMPROMISED_DATA;
        }

        if (width > migif->canvas_width - x || height > migif->canvas_height - y) {
            return EFI_COMPROMISED_DATA;
        }

        if ((UINTN)data_size > frame->payload_size - offset) {
            return EFI_COMPROMISED_DATA;
        }

        EFI_STATUS status = decode_migif_pixels_into_buffer(
            &migif->palette,
            encoding,
            frame->payload + offset,
            (UINTN)data_size,
            width,
            height,
            NULL
        );
        if (EFI_ERROR(status)) {
            return status;
        }

        offset += (UINTN)data_size;
    }

    return offset == frame->payload_size ? EFI_SUCCESS : EFI_COMPROMISED_DATA;
}

static EFI_STATUS validate_migif_transform_payload(const MigifFrame *frame) {
    if (frame->payload_size != MIGIF_TRANSFORM_PAYLOAD_SIZE) {
        return EFI_COMPROMISED_DATA;
    }

    UINT32 transform_flags = read_u32le(frame->payload, 0);
    UINT32 alpha = read_u32le(frame->payload, 12);

    if ((transform_flags & MIGIF_TF_ALPHA) != 0 && alpha > 255) {
        return EFI_COMPROMISED_DATA;
    }

    return EFI_SUCCESS;
}

static EFI_STATUS validate_migif_frame_payload(const MigifFile *migif, const MigifFrame *frame) {
    if (frame->type == MIGIF_FRAME_FULL) {
        return validate_migif_full_payload(migif, frame);
    }

    if (frame->type == MIGIF_FRAME_DELTA) {
        return validate_migif_delta_payload(migif, frame);
    }

    if (frame->type == MIGIF_FRAME_TRANSFORM) {
        return validate_migif_transform_payload(frame);
    }

    return EFI_UNSUPPORTED;
}

static EFI_STATUS parse_migif(
    EFI_SYSTEM_TABLE *SystemTable,
    UINT8 *data,
    UINTN data_size,
    MigifFile *out_migif
) {
    MigifFile migif;
    zero_bytes(&migif, sizeof(migif));
    migif.data = data;
    migif.data_size = data_size;

    if (data_size < MIGIF_FILE_HEADER_SIZE) {
        free_migif_file(SystemTable, &migif);
        return EFI_COMPROMISED_DATA;
    }

    if (data[0] != 'M' || data[1] != 'I' || data[2] != 'G' || data[3] != 'I' || data[4] != 'F') {
        free_migif_file(SystemTable, &migif);
        return EFI_COMPROMISED_DATA;
    }

    migif.version_major = data[5];
    migif.version_minor = data[6];
    UINT8 header_size = data[7];
    migif.flags = data[8];
    migif.canvas_width = read_u32le(data, 9);
    migif.canvas_height = read_u32le(data, 13);
    migif.frame_count = read_u32le(data, 17);
    migif.fps_num = read_u32le(data, 21);
    migif.fps_den = read_u32le(data, 25);
    UINT32 blocks_size = read_u32le(data, 29);
    UINT32 frames_offset = read_u32le(data, 33);
    UINT32 file_size = read_u32le(data, 37);

    if ((migif.version_major != 1 && migif.version_major != 2)
        || header_size < MIGIF_FILE_HEADER_SIZE
        || migif.fps_den == 0
        || migif.canvas_width == 0
        || migif.canvas_height == 0
        || migif.frame_count == 0
        || file_size != data_size
        || header_size > data_size
        || frames_offset > data_size) {
        free_migif_file(SystemTable, &migif);
        return EFI_COMPROMISED_DATA;
    }

    EFI_STATUS status = get_pixel_count(migif.canvas_width, migif.canvas_height, &migif.canvas_pixel_count);
    if (EFI_ERROR(status)) {
        free_migif_file(SystemTable, &migif);
        return status;
    }

    UINT64 block_area_end = (UINT64)header_size + (UINT64)blocks_size;
    if (block_area_end > data_size || (UINT64)frames_offset != block_area_end) {
        free_migif_file(SystemTable, &migif);
        return EFI_COMPROMISED_DATA;
    }

    status = allocate_pool(
        SystemTable,
        (UINTN)migif.frame_count * sizeof(MigifFrame),
        (VOID **)&migif.frames
    );
    if (EFI_ERROR(status)) {
        free_migif_file(SystemTable, &migif);
        return status;
    }
    zero_bytes(migif.frames, (UINTN)migif.frame_count * sizeof(MigifFrame));

    status = allocate_pool(
        SystemTable,
        (UINTN)migif.frame_count * sizeof(UINT64),
        (VOID **)&migif.frame_start_times
    );
    if (EFI_ERROR(status)) {
        free_migif_file(SystemTable, &migif);
        return status;
    }
    zero_bytes(migif.frame_start_times, (UINTN)migif.frame_count * sizeof(UINT64));

    UINTN offset = (UINTN)header_size;
    UINTN blocks_end = (UINTN)block_area_end;
    while (offset < blocks_end) {
        if (offset + MIGIF_BLOCK_HEADER_SIZE > blocks_end) {
            free_migif_file(SystemTable, &migif);
            return EFI_COMPROMISED_DATA;
        }

        UINT32 block_size = read_u32le(data, offset + 4);
        if (block_size < MIGIF_BLOCK_HEADER_SIZE || offset + block_size > blocks_end) {
            free_migif_file(SystemTable, &migif);
            return EFI_COMPROMISED_DATA;
        }

        if (data[offset + 0] == 'P'
            && data[offset + 1] == 'L'
            && data[offset + 2] == 'T'
            && data[offset + 3] == 'E') {
            status = validate_plte_block(
                SystemTable,
                data + offset + MIGIF_BLOCK_HEADER_SIZE,
                block_size - MIGIF_BLOCK_HEADER_SIZE,
                &migif.palette
            );
            if (EFI_ERROR(status)) {
                free_migif_file(SystemTable, &migif);
                return status;
            }
        }

        offset += block_size;
    }

    if (offset != blocks_end) {
        free_migif_file(SystemTable, &migif);
        return EFI_COMPROMISED_DATA;
    }

    UINT32 previous_full = INVALID_FRAME_INDEX;
    UINT64 total_duration = 0;
    offset = frames_offset;
    for (UINT32 i = 0; i < migif.frame_count; i++) {
        if (offset + MIGIF_FRAME_HEADER_SIZE > data_size) {
            free_migif_file(SystemTable, &migif);
            return EFI_COMPROMISED_DATA;
        }

        MigifFrame *frame = &migif.frames[i];
        UINT32 frame_size = read_u32le(data, offset + 0);
        frame->type = read_u16le(data, offset + 4);
        frame->flags = read_u16le(data, offset + 6);
        frame->duration_num = read_u32le(data, offset + 8);
        frame->duration_den = read_u32le(data, offset + 12);
        frame->payload_size = read_u32le(data, offset + 16);
        frame->reserved0 = read_u32le(data, offset + 20);
        frame->reserved1 = read_u32le(data, offset + 24);
        frame->payload = data + offset + MIGIF_FRAME_HEADER_SIZE;
        frame->full_reset_index = previous_full;

        if (frame_size < MIGIF_FRAME_HEADER_SIZE
            || frame->duration_den == 0
            || frame_size != MIGIF_FRAME_HEADER_SIZE + frame->payload_size
            || offset + frame_size > data_size) {
            free_migif_file(SystemTable, &migif);
            return EFI_COMPROMISED_DATA;
        }

        status = migif_duration_to_100ns(frame->duration_num, frame->duration_den, &frame->duration_100ns);
        if (EFI_ERROR(status)) {
            free_migif_file(SystemTable, &migif);
            return status;
        }

        migif.frame_start_times[i] = total_duration;
        total_duration += frame->duration_100ns;

        if (frame->type == MIGIF_FRAME_FULL) {
            frame->full_reset_index = i;
            previous_full = i;
        }

        status = validate_migif_frame_payload(&migif, frame);
        if (EFI_ERROR(status)) {
            free_migif_file(SystemTable, &migif);
            return status;
        }

        offset += frame_size;
    }

    if (offset != data_size) {
        free_migif_file(SystemTable, &migif);
        return EFI_COMPROMISED_DATA;
    }

    migif.total_duration_100ns = total_duration;
    *out_migif = migif;
    return EFI_SUCCESS;
}

static EFI_STATUS migif_copy_region_to_canvas(
    const EFI_GRAPHICS_OUTPUT_BLT_PIXEL *src_pixels,
    UINT32 src_width,
    UINT32 src_height,
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *canvas,
    UINT32 canvas_width,
    UINT32 dst_x,
    UINT32 dst_y
) {
    for (UINT32 y = 0; y < src_height; y++) {
        UINTN dst_row = ((UINTN)(dst_y + y) * (UINTN)canvas_width) + (UINTN)dst_x;
        UINTN src_row = (UINTN)y * (UINTN)src_width;
        for (UINT32 x = 0; x < src_width; x++) {
            canvas[dst_row + x] = src_pixels[src_row + x];
        }
    }

    return EFI_SUCCESS;
}

static EFI_STATUS apply_migif_full_frame(
    EFI_SYSTEM_TABLE *SystemTable,
    const MigifFile *migif,
    MigifPlayerState *player,
    const MigifFrame *frame
) {
    UINT16 encoding = read_u16le(frame->payload, 0);
    UINT32 width = read_u32le(frame->payload, 4);
    UINT32 height = read_u32le(frame->payload, 8);
    UINT32 data_size = read_u32le(frame->payload, 12);

    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *decoded = NULL;
    EFI_STATUS status = decode_migif_pixels(
        SystemTable,
        &migif->palette,
        encoding,
        frame->payload + MIGIF_FULL_PAYLOAD_HEADER_SIZE,
        (UINTN)data_size,
        width,
        height,
        &decoded
    );
    if (EFI_ERROR(status)) {
        return status;
    }

    EFI_GRAPHICS_OUTPUT_BLT_PIXEL transparent = { 0, 0, 0, 0 };
    fill_pixel_buffer(player->canvas, player->canvas_pixel_count, transparent);
    migif_copy_region_to_canvas(decoded, width, height, player->canvas, migif->canvas_width, 0, 0);
    migif_transform_identity(&player->transform);

    free_pool(SystemTable, decoded);
    return EFI_SUCCESS;
}

static EFI_STATUS apply_migif_delta_frame(
    EFI_SYSTEM_TABLE *SystemTable,
    const MigifFile *migif,
    MigifPlayerState *player,
    const MigifFrame *frame
) {
    UINT16 encoding = read_u16le(frame->payload, 0);
    UINT16 rect_count = read_u16le(frame->payload, 2);
    UINTN offset = MIGIF_DELTA_PAYLOAD_HEADER_SIZE;

    for (UINT16 rect_index = 0; rect_index < rect_count; rect_index++) {
        UINT32 x = read_u32le(frame->payload, offset + 0);
        UINT32 y = read_u32le(frame->payload, offset + 4);
        UINT32 width = read_u32le(frame->payload, offset + 8);
        UINT32 height = read_u32le(frame->payload, offset + 12);
        UINT32 data_size = read_u32le(frame->payload, offset + 16);
        offset += MIGIF_DELTA_RECT_HEADER_SIZE;

        EFI_GRAPHICS_OUTPUT_BLT_PIXEL *decoded = NULL;
        EFI_STATUS status = decode_migif_pixels(
            SystemTable,
            &migif->palette,
            encoding,
            frame->payload + offset,
            (UINTN)data_size,
            width,
            height,
            &decoded
        );
        if (EFI_ERROR(status)) {
            return status;
        }

        migif_copy_region_to_canvas(decoded, width, height, player->canvas, migif->canvas_width, x, y);
        free_pool(SystemTable, decoded);
        offset += (UINTN)data_size;
    }

    return EFI_SUCCESS;
}

static EFI_STATUS apply_migif_transform_frame(
    const MigifFrame *frame,
    MigifPlayerState *player
) {
    UINT32 transform_flags = read_u32le(frame->payload, 0);

    if ((transform_flags & MIGIF_TF_POS_X) != 0) {
        player->transform.pos_x = read_s32le(frame->payload, 4);
    }

    if ((transform_flags & MIGIF_TF_POS_Y) != 0) {
        player->transform.pos_y = read_s32le(frame->payload, 8);
    }

    if ((transform_flags & MIGIF_TF_ALPHA) != 0) {
        player->transform.alpha = read_u32le(frame->payload, 12);
    }

    if ((transform_flags & MIGIF_TF_SCALE_X) != 0) {
        player->transform.scale_x = read_s32le(frame->payload, 16);
    }

    if ((transform_flags & MIGIF_TF_SCALE_Y) != 0) {
        player->transform.scale_y = read_s32le(frame->payload, 20);
    }

    if ((transform_flags & MIGIF_TF_ROTATION) != 0) {
        player->transform.rotation_deg = read_s32le(frame->payload, 24);
    }

    return EFI_SUCCESS;
}

static EFI_STATUS apply_migif_frame(
    EFI_SYSTEM_TABLE *SystemTable,
    const MigifFile *migif,
    MigifPlayerState *player,
    UINT32 frame_index
) {
    const MigifFrame *frame = &migif->frames[frame_index];

    if (frame->type == MIGIF_FRAME_FULL) {
        return apply_migif_full_frame(SystemTable, migif, player, frame);
    }

    if (frame->type == MIGIF_FRAME_DELTA) {
        return apply_migif_delta_frame(SystemTable, migif, player, frame);
    }

    if (frame->type == MIGIF_FRAME_TRANSFORM) {
        return apply_migif_transform_frame(frame, player);
    }

    return EFI_UNSUPPORTED;
}

static EFI_STATUS rebuild_migif_state(
    EFI_SYSTEM_TABLE *SystemTable,
    const MigifFile *migif,
    MigifPlayerState *player,
    UINT32 target_frame
) {
    if (target_frame >= migif->frame_count) {
        return EFI_INVALID_PARAMETER;
    }

    EFI_GRAPHICS_OUTPUT_BLT_PIXEL transparent = { 0, 0, 0, 0 };
    fill_pixel_buffer(player->canvas, player->canvas_pixel_count, transparent);
    migif_transform_identity(&player->transform);

    UINT32 start_frame = 0;
    if (migif->frames[target_frame].full_reset_index != INVALID_FRAME_INDEX) {
        start_frame = migif->frames[target_frame].full_reset_index;
    }

    for (UINT32 frame_index = start_frame; frame_index <= target_frame; frame_index++) {
        EFI_STATUS status = apply_migif_frame(SystemTable, migif, player, frame_index);
        if (EFI_ERROR(status)) {
            return status;
        }
    }

    player->current_frame = target_frame;
    return EFI_SUCCESS;
}

static EFI_STATUS jump_to_migif_frame(
    EFI_SYSTEM_TABLE *SystemTable,
    const MigifFile *migif,
    MigifPlayerState *player,
    UINT32 frame_index,
    UINT64 frame_elapsed_100ns
) {
    if (migif->frame_cache != NULL) {
        if (frame_index >= migif->frame_count) {
            return EFI_INVALID_PARAMETER;
        }

        player->current_frame = frame_index;
        player->frame_elapsed_100ns = frame_elapsed_100ns;
        player->reached_end = FALSE;
        player->force_full_redraw = TRUE;
        return EFI_SUCCESS;
    }

    EFI_STATUS status = rebuild_migif_state(SystemTable, migif, player, frame_index);
    if (EFI_ERROR(status)) {
        return status;
    }

    player->current_frame = frame_index;
    player->frame_elapsed_100ns = frame_elapsed_100ns;
    player->reached_end = FALSE;
    player->force_full_redraw = TRUE;
    return EFI_SUCCESS;
}

static UINT64 current_migif_time(const MigifFile *migif, const MigifPlayerState *player) {
    if (player->reached_end) {
        return migif->total_duration_100ns;
    }

    UINT64 frame_start = migif->frame_start_times[player->current_frame];
    UINT64 frame_duration = migif->frames[player->current_frame].duration_100ns;
    UINT64 elapsed = player->frame_elapsed_100ns;

    if (elapsed > frame_duration) {
        elapsed = frame_duration;
    }

    return frame_start + elapsed;
}

static void locate_migif_time(
    const MigifFile *migif,
    UINT64 target_time_100ns,
    UINT32 *out_frame_index,
    UINT64 *out_frame_elapsed_100ns
) {
    for (UINT32 i = 0; i < migif->frame_count; i++) {
        UINT64 frame_start = migif->frame_start_times[i];
        UINT64 frame_end = frame_start + migif->frames[i].duration_100ns;

        if (target_time_100ns < frame_end || i + 1 == migif->frame_count) {
            *out_frame_index = i;
            *out_frame_elapsed_100ns = target_time_100ns - frame_start;
            return;
        }
    }

    *out_frame_index = migif->frame_count - 1;
    *out_frame_elapsed_100ns = migif->frames[migif->frame_count - 1].duration_100ns;
}

static EFI_STATUS seek_migif_relative(
    EFI_SYSTEM_TABLE *SystemTable,
    const MigifFile *migif,
    MigifPlayerState *player,
    INT64 delta_100ns
) {
    if (migif->total_duration_100ns == 0) {
        return jump_to_migif_frame(SystemTable, migif, player, 0, 0);
    }

    UINT64 current_time = current_migif_time(migif, player);
    INT64 target_time = (INT64)current_time + delta_100ns;

    if (target_time < 0) {
        target_time = 0;
    }

    if (migif->total_duration_100ns > 0 && (UINT64)target_time >= migif->total_duration_100ns) {
        target_time = (INT64)(migif->total_duration_100ns - 1);
    }

    UINT32 frame_index = 0;
    UINT64 frame_elapsed = 0;
    locate_migif_time(migif, (UINT64)target_time, &frame_index, &frame_elapsed);
    return jump_to_migif_frame(SystemTable, migif, player, frame_index, frame_elapsed);
}

static EFI_STATUS step_migif_frame(
    EFI_SYSTEM_TABLE *SystemTable,
    const MigifFile *migif,
    MigifPlayerState *player,
    INTN delta
) {
    INTN target = (INTN)player->current_frame + delta;
    if (target < 0) {
        target = 0;
    }

    if ((UINT32)target >= migif->frame_count) {
        target = (INTN)(migif->frame_count - 1);
    }

    return jump_to_migif_frame(SystemTable, migif, player, (UINT32)target, 0);
}

static EFI_STATUS advance_migif_playback(
    EFI_SYSTEM_TABLE *SystemTable,
    const MigifFile *migif,
    MigifPlayerState *player,
    UINT64 elapsed_100ns,
    BOOLEAN *out_render_needed
) {
    *out_render_needed = FALSE;

    if (player->paused || player->reached_end) {
        return EFI_SUCCESS;
    }

    player->frame_elapsed_100ns += elapsed_100ns;

    UINTN safety = (UINTN)migif->frame_count + 1;
    while (safety-- > 0) {
        UINT64 current_duration = migif->frames[player->current_frame].duration_100ns;
        if (player->frame_elapsed_100ns < current_duration) {
            return EFI_SUCCESS;
        }

        if (player->current_frame + 1 < migif->frame_count) {
            player->frame_elapsed_100ns -= current_duration;
            player->current_frame++;
            if (migif->frame_cache == NULL) {
                EFI_STATUS status = apply_migif_frame(SystemTable, migif, player, player->current_frame);
                if (EFI_ERROR(status)) {
                    return status;
                }
            }
            *out_render_needed = TRUE;
            continue;
        }

        if ((migif->flags & MIGIF_FLAG_LOOP) != 0) {
            player->frame_elapsed_100ns -= current_duration;
            if (migif->frame_cache != NULL) {
                player->current_frame = 0;
                player->frame_elapsed_100ns = 0;
                player->reached_end = FALSE;
                player->force_full_redraw = FALSE;
            } else {
                EFI_STATUS status = jump_to_migif_frame(SystemTable, migif, player, 0, 0);
                if (EFI_ERROR(status)) {
                    return status;
                }
            }
            *out_render_needed = TRUE;
            continue;
        }

        player->frame_elapsed_100ns = current_duration;
        player->paused = TRUE;
        player->reached_end = TRUE;
        return EFI_SUCCESS;
    }

    return EFI_COMPROMISED_DATA;
}

static EFI_STATUS init_migif_player(
    EFI_SYSTEM_TABLE *SystemTable,
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
    const MigifFile *migif,
    MigifPlayerState *player
) {
    (void)gop;

    zero_bytes(player, sizeof(*player));
    migif_transform_identity(&player->transform);
    player->paused = FALSE;
    player->reached_end = FALSE;

    EFI_STATUS status = allocate_pixel_buffer(SystemTable, migif->canvas_width, migif->canvas_height, &player->canvas);
    if (EFI_ERROR(status)) {
        return status;
    }

    status = get_pixel_count(migif->canvas_width, migif->canvas_height, &player->canvas_pixel_count);
    if (EFI_ERROR(status)) {
        free_migif_player(SystemTable, player);
        return status;
    }

    status = allocate_pixel_buffer(SystemTable, migif->canvas_width, migif->canvas_height, &player->screen_buffer);
    if (EFI_ERROR(status)) {
        free_migif_player(SystemTable, player);
        return status;
    }

    return jump_to_migif_frame(SystemTable, migif, player, 0, 0);
}

static void compute_dirty_rect(
    const EFI_GRAPHICS_OUTPUT_BLT_PIXEL *previous_pixels,
    const EFI_GRAPHICS_OUTPUT_BLT_PIXEL *current_pixels,
    UINT32 width,
    UINT32 height,
    MigifDirtyRect *out_rect
) {
    BOOLEAN has_changes = FALSE;
    UINT32 min_x = width;
    UINT32 min_y = height;
    UINT32 max_x = 0;
    UINT32 max_y = 0;

    for (UINT32 y = 0; y < height; y++) {
        UINTN row = (UINTN)y * (UINTN)width;

        for (UINT32 x = 0; x < width; x++) {
            EFI_GRAPHICS_OUTPUT_BLT_PIXEL prev = previous_pixels[row + (UINTN)x];
            EFI_GRAPHICS_OUTPUT_BLT_PIXEL curr = current_pixels[row + (UINTN)x];

            if (prev.Blue == curr.Blue
                && prev.Green == curr.Green
                && prev.Red == curr.Red
                && prev.Reserved == curr.Reserved) {
                continue;
            }

            if (!has_changes) {
                min_x = x;
                max_x = x;
                min_y = y;
                max_y = y;
                has_changes = TRUE;
                continue;
            }

            if (x < min_x) {
                min_x = x;
            }
            if (x > max_x) {
                max_x = x;
            }
            if (y < min_y) {
                min_y = y;
            }
            if (y > max_y) {
                max_y = y;
            }
        }
    }

    if (!has_changes) {
        out_rect->x = 0;
        out_rect->y = 0;
        out_rect->width = 0;
        out_rect->height = 0;
        return;
    }

    out_rect->x = min_x;
    out_rect->y = min_y;
    out_rect->width = max_x - min_x + 1;
    out_rect->height = max_y - min_y + 1;
}

static void show_loading_screen(EFI_SYSTEM_TABLE *SystemTable) {
    uefi_call_wrapper(SystemTable->ConOut->ClearScreen, 1, SystemTable->ConOut);
    PrintLn(L"Loading...");
}

static UINT64 get_monotonic_count_or_zero(EFI_SYSTEM_TABLE *SystemTable) {
    UINT64 count = 0;
    EFI_STATUS status = uefi_call_wrapper(
        SystemTable->BootServices->GetNextMonotonicCount,
        1,
        &count
    );
    if (EFI_ERROR(status)) {
        return 0;
    }

    return count;
}

static BOOLEAN is_space_debounced(EFI_SYSTEM_TABLE *SystemTable, const MigifPlayerState *player) {
    if (player->space_debounce_until == 0) {
        return FALSE;
    }

    UINT64 now = get_monotonic_count_or_zero(SystemTable);
    if (now == 0) {
        return FALSE;
    }

    return now < player->space_debounce_until;
}

static void set_space_debounce(EFI_SYSTEM_TABLE *SystemTable, MigifPlayerState *player) {
    UINT64 now = get_monotonic_count_or_zero(SystemTable);
    if (now == 0) {
        player->space_debounce_until = 0;
        return;
    }

    player->space_debounce_until = now + (UINT64)SPACE_DEBOUNCE_TICKS;
}

static UINT64 migif_frame_remaining_100ns(const MigifFile *migif, const MigifPlayerState *player) {
    UINT64 duration = migif->frames[player->current_frame].duration_100ns;
    if (player->frame_elapsed_100ns >= duration) {
        return 1;
    }

    return duration - player->frame_elapsed_100ns;
}

static EFI_STATUS arm_playback_timer(
    EFI_SYSTEM_TABLE *SystemTable,
    EFI_EVENT tick_event,
    const MigifFile *migif,
    const MigifPlayerState *player
) {
    UINT64 trigger_100ns = 0;
    EFI_TIMER_DELAY timer_type = TimerCancel;

    if (!player->paused && !player->reached_end) {
        timer_type = TimerRelative;
        trigger_100ns = migif_frame_remaining_100ns(migif, player);
    }

    return uefi_call_wrapper(SystemTable->BootServices->SetTimer, 3, tick_event, timer_type, trigger_100ns);
}

static void compose_migif_frame_to_buffer(
    const MigifFile *migif,
    const MigifPlayerState *player,
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *out_pixels
) {
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL black = { 0, 0, 0, 0 };
    UINT32 global_alpha = player->transform.alpha;

    for (UINTN i = 0; i < migif->canvas_pixel_count; i++) {
        EFI_GRAPHICS_OUTPUT_BLT_PIXEL source = player->canvas[i];
        UINTN alpha = ((UINTN)source.Reserved * (UINTN)global_alpha + 127U) / 255U;

        EFI_GRAPHICS_OUTPUT_BLT_PIXEL output = black;
        if (alpha != 0) {
            output.Red = (UINT8)(((UINTN)source.Red * alpha + 127U) / 255U);
            output.Green = (UINT8)(((UINTN)source.Green * alpha + 127U) / 255U);
            output.Blue = (UINT8)(((UINTN)source.Blue * alpha + 127U) / 255U);
        }

        out_pixels[i] = output;
    }
}

static EFI_STATUS cache_migif_frames(
    EFI_SYSTEM_TABLE *SystemTable,
    const MigifFile *migif,
    MigifPlayerState *player
) {
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL **frame_cache = NULL;
    MigifDirtyRect *frame_dirty_rects = NULL;
    EFI_STATUS status = allocate_pool(
        SystemTable,
        (UINTN)migif->frame_count * sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL *),
        (VOID **)&frame_cache
    );
    if (EFI_ERROR(status)) {
        return status;
    }

    zero_bytes(frame_cache, (UINTN)migif->frame_count * sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL *));

    status = allocate_pool(
        SystemTable,
        (UINTN)migif->frame_count * sizeof(MigifDirtyRect),
        (VOID **)&frame_dirty_rects
    );
    if (EFI_ERROR(status)) {
        free_pool(SystemTable, frame_cache);
        return status;
    }

    zero_bytes(frame_dirty_rects, (UINTN)migif->frame_count * sizeof(MigifDirtyRect));

    for (UINT32 frame_index = 0; frame_index < migif->frame_count; frame_index++) {
        EFI_GRAPHICS_OUTPUT_BLT_PIXEL *frame_pixels = NULL;
        status = allocate_pixel_buffer(
            SystemTable,
            migif->canvas_width,
            migif->canvas_height,
            &frame_pixels
        );
        if (EFI_ERROR(status)) {
            for (UINT32 i = 0; i < migif->frame_count; i++) {
                free_pool(SystemTable, frame_cache[i]);
            }
            free_pool(SystemTable, frame_dirty_rects);
            free_pool(SystemTable, frame_cache);
            return status;
        }

        if (frame_index > 0) {
            status = apply_migif_frame(SystemTable, migif, player, frame_index);
            if (EFI_ERROR(status)) {
                free_pool(SystemTable, frame_pixels);
                for (UINT32 i = 0; i < migif->frame_count; i++) {
                    free_pool(SystemTable, frame_cache[i]);
                }
                free_pool(SystemTable, frame_dirty_rects);
                free_pool(SystemTable, frame_cache);
                return status;
            }
        }

        compose_migif_frame_to_buffer(migif, player, frame_pixels);
        frame_cache[frame_index] = frame_pixels;

        if (frame_index == 0) {
            frame_dirty_rects[frame_index].x = 0;
            frame_dirty_rects[frame_index].y = 0;
            frame_dirty_rects[frame_index].width = migif->canvas_width;
            frame_dirty_rects[frame_index].height = migif->canvas_height;
        } else {
            compute_dirty_rect(
                frame_cache[frame_index - 1],
                frame_cache[frame_index],
                migif->canvas_width,
                migif->canvas_height,
                &frame_dirty_rects[frame_index]
            );
        }
    }

    ((MigifFile *)migif)->frame_cache = frame_cache;
    ((MigifFile *)migif)->frame_dirty_rects = frame_dirty_rects;
    if (migif->frame_count > 1) {
        compute_dirty_rect(
            frame_cache[migif->frame_count - 1],
            frame_cache[0],
            migif->canvas_width,
            migif->canvas_height,
            &((MigifFile *)migif)->loop_dirty_rect
        );
    } else {
        ((MigifFile *)migif)->loop_dirty_rect = frame_dirty_rects[0];
    }
    player->current_frame = 0;
    player->frame_elapsed_100ns = 0;
    player->reached_end = FALSE;
    player->has_rendered_frame = FALSE;
    player->force_full_redraw = TRUE;
    player->last_rendered_frame = 0;
    return EFI_SUCCESS;
}

static BOOLEAN playback_input_has_shift(const PlaybackInput *input) {
    if (!input->has_extended_state) {
        return FALSE;
    }

    if ((input->key_state.KeyShiftState & EFI_SHIFT_STATE_VALID) == 0) {
        return FALSE;
    }

    return (input->key_state.KeyShiftState & (EFI_LEFT_SHIFT_PRESSED | EFI_RIGHT_SHIFT_PRESSED)) != 0;
}

static EFI_STATUS read_playback_input(
    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *input_ex,
    PlaybackInput *out_input
) {
    EFI_KEY_DATA key_data;
    zero_bytes(&key_data, sizeof(key_data));

    EFI_STATUS status = uefi_call_wrapper(input_ex->ReadKeyStrokeEx, 2, input_ex, &key_data);
    if (EFI_ERROR(status)) {
        return status;
    }

    out_input->key = key_data.Key;
    out_input->key_state = key_data.KeyState;
    out_input->has_extended_state = TRUE;
    return EFI_SUCCESS;
}

static EFI_STATUS render_migif_player(
    EFI_SYSTEM_TABLE *SystemTable,
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
    const MigifFile *migif,
    MigifPlayerState *player
) {
    UINT32 screen_width = gop->Mode->Info->HorizontalResolution;
    UINT32 screen_height = gop->Mode->Info->VerticalResolution;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL black = { 0, 0, 0, 0 };
    INT64 base_x = ((INT64)screen_width - (INT64)migif->canvas_width) / 2;
    INT64 base_y = ((INT64)screen_height - (INT64)migif->canvas_height) / 2;

    INT64 source_x = 0;
    INT64 source_y = 0;
    INT64 draw_width = migif->canvas_width;
    INT64 draw_height = migif->canvas_height;

    if (migif->frame_cache != NULL) {
        EFI_STATUS status = render_centered_faded_pixels(
            SystemTable,
            gop,
            migif->frame_cache[player->current_frame],
            migif->canvas_width,
            migif->canvas_height,
            255
        );
        if (!EFI_ERROR(status)) {
            player->last_rendered_frame = player->current_frame;
            player->has_rendered_frame = TRUE;
            player->force_full_redraw = FALSE;
        }
        return status;
    }

    INT64 visible_x = base_x + source_x;
    INT64 visible_y = base_y + source_y;

    if (visible_x >= (INT64)screen_width || visible_y >= (INT64)screen_height || draw_width <= 0 || draw_height <= 0) {
        player->last_rendered_frame = player->current_frame;
        player->has_rendered_frame = TRUE;
        player->force_full_redraw = FALSE;
        return EFI_SUCCESS;
    }

    if (visible_x < 0) {
        source_x += -visible_x;
        draw_width -= -visible_x;
        visible_x = 0;
    }

    if (visible_y < 0) {
        source_y += -visible_y;
        draw_height -= -visible_y;
        visible_y = 0;
    }

    if (visible_x + draw_width > (INT64)screen_width) {
        draw_width = (INT64)screen_width - visible_x;
    }

    if (visible_y + draw_height > (INT64)screen_height) {
        draw_height = (INT64)screen_height - visible_y;
    }

    if (draw_width <= 0 || draw_height <= 0) {
        player->last_rendered_frame = player->current_frame;
        player->has_rendered_frame = TRUE;
        player->force_full_redraw = FALSE;
        return EFI_SUCCESS;
    }

    EFI_STATUS status = fill_rect(
        gop,
        black,
        (UINTN)visible_x,
        (UINTN)visible_y,
        (UINTN)draw_width,
        (UINTN)draw_height
    );
    if (EFI_ERROR(status)) {
        return status;
    }

    if (player->transform.alpha == 0) {
        player->last_rendered_frame = player->current_frame;
        player->has_rendered_frame = TRUE;
        player->force_full_redraw = FALSE;
        return EFI_SUCCESS;
    }

    for (UINTN y = 0; y < (UINTN)draw_height; y++) {
        UINTN src_y = (UINTN)source_y + y;
        UINTN row = src_y * (UINTN)migif->canvas_width;

        for (UINTN x = 0; x < (UINTN)draw_width; x++) {
            UINTN src_x = (UINTN)source_x + x;
            EFI_GRAPHICS_OUTPUT_BLT_PIXEL source = player->canvas[row + src_x];
            UINTN alpha = ((UINTN)source.Reserved * (UINTN)player->transform.alpha + 127U) / 255U;

            EFI_GRAPHICS_OUTPUT_BLT_PIXEL output = black;
            if (alpha != 0) {
                output.Red = (UINT8)(((UINTN)source.Red * alpha + 127U) / 255U);
                output.Green = (UINT8)(((UINTN)source.Green * alpha + 127U) / 255U);
                output.Blue = (UINT8)(((UINTN)source.Blue * alpha + 127U) / 255U);
            }

            player->screen_buffer[row + src_x] = output;
        }
    }

    status = draw_blt_subimage(
        gop,
        player->screen_buffer,
        migif->canvas_width,
        (UINTN)source_x,
        (UINTN)source_y,
        (UINTN)draw_width,
        (UINTN)draw_height,
        (UINTN)visible_x,
        (UINTN)visible_y
    );
    if (!EFI_ERROR(status)) {
        player->last_rendered_frame = player->current_frame;
        player->has_rendered_frame = TRUE;
        player->force_full_redraw = FALSE;
    }
    return status;
}

static EFI_STATUS handle_playback_input(
    EFI_SYSTEM_TABLE *SystemTable,
    const MigifFile *migif,
    MigifPlayerState *player,
    const PlaybackInput *input,
    BOOLEAN *out_exit,
    BOOLEAN *out_render
) {
    *out_exit = FALSE;
    *out_render = FALSE;

    if (input->key.UnicodeChar == L'q' || input->key.UnicodeChar == L'Q') {
        *out_exit = TRUE;
        return EFI_SUCCESS;
    }

    if (input->key.UnicodeChar == L' ') {
        if (is_space_debounced(SystemTable, player)) {
            return EFI_SUCCESS;
        }

        set_space_debounce(SystemTable, player);
        if (player->reached_end && player->current_frame + 1 == migif->frame_count) {
            EFI_STATUS status = jump_to_migif_frame(SystemTable, migif, player, 0, 0);
            if (EFI_ERROR(status)) {
                return status;
            }
            player->paused = FALSE;
            *out_render = TRUE;
            return EFI_SUCCESS;
        }

        player->paused = !player->paused;
        if (!player->paused) {
            player->reached_end = FALSE;
        }
        return EFI_SUCCESS;
    }

    if (input->key.ScanCode == SCAN_LEFT || input->key.ScanCode == SCAN_RIGHT) {
        INTN direction = input->key.ScanCode == SCAN_LEFT ? -1 : 1;

        if (playback_input_has_shift(input)) {
            EFI_STATUS status = step_migif_frame(SystemTable, migif, player, direction);
            if (EFI_ERROR(status)) {
                return status;
            }
            player->paused = TRUE;
            *out_render = TRUE;
            return EFI_SUCCESS;
        }

        BOOLEAN was_paused = player->paused;
        EFI_STATUS status = seek_migif_relative(
            SystemTable,
            migif,
            player,
            direction < 0 ? -(INT64)SEEK_STEP_100NS : (INT64)SEEK_STEP_100NS
        );
        if (EFI_ERROR(status)) {
            return status;
        }

        player->paused = was_paused;
        *out_render = TRUE;
        return EFI_SUCCESS;
    }

    return EFI_SUCCESS;
}

static EFI_STATUS load_miimg_asset(
    EFI_SYSTEM_TABLE *SystemTable,
    const CHAR16 *asset_name,
    DecodedImage *image
) {
    EFI_FILE_HANDLE file = NULL;
    EFI_STATUS status = open_asset_file(SystemTable, asset_name, L".miimg", &file);
    if (EFI_ERROR(status)) {
        return status;
    }

    UINT8 *file_data = NULL;
    UINTN file_size = 0;
    status = read_entire_file(SystemTable, file, &file_data, &file_size);
    close_file(file);
    if (EFI_ERROR(status)) {
        return status;
    }

    status = decode_miimg(SystemTable, file_data, file_size, image);
    free_pool(SystemTable, file_data);
    return status;
}

static EFI_STATUS load_migif_asset(
    EFI_SYSTEM_TABLE *SystemTable,
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
    const CHAR16 *asset_name,
    MigifFile *migif,
    MigifPlayerState *player
) {
    EFI_FILE_HANDLE file = NULL;
    EFI_STATUS status = open_asset_file(SystemTable, asset_name, L".migif", &file);
    if (EFI_ERROR(status)) {
        return status;
    }

    UINT8 *file_data = NULL;
    UINTN file_size = 0;
    status = read_entire_file(SystemTable, file, &file_data, &file_size);
    close_file(file);
    if (EFI_ERROR(status)) {
        return status;
    }

    zero_bytes(migif, sizeof(*migif));
    status = parse_migif(SystemTable, file_data, file_size, migif);
    if (EFI_ERROR(status)) {
        return status;
    }

    status = init_migif_player(SystemTable, gop, migif, player);
    if (EFI_ERROR(status)) {
        free_migif_file(SystemTable, migif);
        return status;
    }

    status = cache_migif_frames(SystemTable, migif, player);
    if (EFI_ERROR(status)) {
        free_migif_player(SystemTable, player);
        free_migif_file(SystemTable, migif);
        return status;
    }

    return EFI_SUCCESS;
}

static EFI_STATUS render_centered_alpha_pixels(
    EFI_SYSTEM_TABLE *SystemTable,
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
    const EFI_GRAPHICS_OUTPUT_BLT_PIXEL *pixels,
    UINT32 width,
    UINT32 height,
    UINT8 global_alpha
) {
    UINT32 screen_width = gop->Mode->Info->HorizontalResolution;
    UINT32 screen_height = gop->Mode->Info->VerticalResolution;

    if (width > screen_width || height > screen_height) {
        return EFI_UNSUPPORTED;
    }

    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *buffer = NULL;
    EFI_STATUS status = allocate_pixel_buffer(SystemTable, width, height, &buffer);
    if (EFI_ERROR(status)) {
        return status;
    }

    UINTN pixel_count = 0;
    status = get_pixel_count(width, height, &pixel_count);
    if (EFI_ERROR(status)) {
        free_pool(SystemTable, buffer);
        return status;
    }

    EFI_GRAPHICS_OUTPUT_BLT_PIXEL black = { 0, 0, 0, 0 };
    for (UINTN i = 0; i < pixel_count; i++) {
        EFI_GRAPHICS_OUTPUT_BLT_PIXEL source = pixels[i];
        UINTN alpha = ((UINTN)source.Reserved * (UINTN)global_alpha + 127U) / 255U;

        EFI_GRAPHICS_OUTPUT_BLT_PIXEL output = black;
        if (alpha != 0) {
            output.Red = (UINT8)(((UINTN)source.Red * alpha + 127U) / 255U);
            output.Green = (UINT8)(((UINTN)source.Green * alpha + 127U) / 255U);
            output.Blue = (UINT8)(((UINTN)source.Blue * alpha + 127U) / 255U);
        }

        buffer[i] = output;
    }

    status = draw_image_centered(gop, buffer, width, height);
    free_pool(SystemTable, buffer);
    return status;
}

static EFI_STATUS render_centered_alpha_image(
    EFI_SYSTEM_TABLE *SystemTable,
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
    const DecodedImage *image,
    UINT8 global_alpha
) {
    return render_centered_alpha_pixels(
        SystemTable,
        gop,
        image->pixels,
        image->width,
        image->height,
        global_alpha
    );
}

static EFI_STATUS render_centered_faded_pixels(
    EFI_SYSTEM_TABLE *SystemTable,
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
    const EFI_GRAPHICS_OUTPUT_BLT_PIXEL *pixels,
    UINT32 width,
    UINT32 height,
    UINT8 global_alpha
) {
    UINT32 screen_width = gop->Mode->Info->HorizontalResolution;
    UINT32 screen_height = gop->Mode->Info->VerticalResolution;

    if (width > screen_width || height > screen_height) {
        return EFI_UNSUPPORTED;
    }

    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *buffer = NULL;
    EFI_STATUS status = allocate_pixel_buffer(SystemTable, width, height, &buffer);
    if (EFI_ERROR(status)) {
        return status;
    }

    UINTN pixel_count = 0;
    status = get_pixel_count(width, height, &pixel_count);
    if (EFI_ERROR(status)) {
        free_pool(SystemTable, buffer);
        return status;
    }

    for (UINTN i = 0; i < pixel_count; i++) {
        EFI_GRAPHICS_OUTPUT_BLT_PIXEL source = pixels[i];
        EFI_GRAPHICS_OUTPUT_BLT_PIXEL output;
        output.Red = (UINT8)(((UINTN)source.Red * (UINTN)global_alpha + 127U) / 255U);
        output.Green = (UINT8)(((UINTN)source.Green * (UINTN)global_alpha + 127U) / 255U);
        output.Blue = (UINT8)(((UINTN)source.Blue * (UINTN)global_alpha + 127U) / 255U);
        output.Reserved = 0;
        buffer[i] = output;
    }

    status = draw_image_centered(gop, buffer, width, height);
    free_pool(SystemTable, buffer);
    return status;
}

static EFI_STATUS run_image_splash(
    EFI_SYSTEM_TABLE *SystemTable,
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
    const DecodedImage *image
) {
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL black = { 0, 0, 0, 0 };
    EFI_STATUS status = fill_screen(gop, black);
    if (!EFI_ERROR(status)) {
        for (UINTN step = 0; step <= SPLASH_FADE_STEPS; step++) {
            UINT8 alpha = (UINT8)((step * 255U) / SPLASH_FADE_STEPS);
            status = render_centered_alpha_image(SystemTable, gop, image, alpha);
            if (EFI_ERROR(status)) {
                break;
            }

            if (step < SPLASH_FADE_STEPS) {
                uefi_call_wrapper(SystemTable->BootServices->Stall, 1, SPLASH_STEP_US);
            }
        }
    }

    if (!EFI_ERROR(status)) {
        uefi_call_wrapper(SystemTable->BootServices->Stall, 1, SPLASH_HOLD_STEPS * SPLASH_STEP_US);
    }

    return status;
}

static EFI_STATUS run_video_splash(
    EFI_SYSTEM_TABLE *SystemTable,
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
    const MigifFile *migif,
    MigifPlayerState *player
) {
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL black = { 0, 0, 0, 0 };
    EFI_STATUS status = fill_screen(gop, black);
    if (EFI_ERROR(status)) {
        return status;
    }

    UINTN total_steps = SPLASH_FADE_STEPS + SPLASH_HOLD_STEPS;
    for (UINTN step = 0; step <= total_steps; step++) {
        UINT8 alpha = 255;
        if (step <= SPLASH_FADE_STEPS) {
            alpha = (UINT8)((step * 255U) / SPLASH_FADE_STEPS);
        }

        status = render_centered_faded_pixels(
            SystemTable,
            gop,
            migif->frame_cache[player->current_frame],
            migif->canvas_width,
            migif->canvas_height,
            alpha
        );
        if (EFI_ERROR(status)) {
            return status;
        }

        if (step == total_steps) {
            break;
        }

        BOOLEAN render_needed = FALSE;
        status = advance_migif_playback(SystemTable, migif, player, SPLASH_STEP_100NS, &render_needed);
        if (EFI_ERROR(status)) {
            return status;
        }

        uefi_call_wrapper(SystemTable->BootServices->Stall, 1, SPLASH_STEP_US);
    }

    return EFI_SUCCESS;
}

static void show_boot_splash(EFI_SYSTEM_TABLE *SystemTable) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_STATUS status = get_gop(SystemTable, &gop);
    if (EFI_ERROR(status) || gop == NULL || gop->Mode == NULL || gop->Mode->Info == NULL) {
        return;
    }

    DecodedImage logo = { 0, 0, NULL };
    status = load_miimg_asset(SystemTable, MIOS_SPLASH_ASSET, &logo);
    if (EFI_ERROR(status)) {
        return;
    }

    status = run_image_splash(SystemTable, gop, &logo);
    free_decoded_image(SystemTable, &logo);

    (void)status;
}

static void run_draw_asset(EFI_SYSTEM_TABLE *SystemTable, const CHAR16 *asset_name) {
    if (asset_name == NULL || asset_name[0] == L'\0') {
        PrintLn(L"Usage: draw <asset>");
        return;
    }

    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_STATUS status = get_gop(SystemTable, &gop);
    if (EFI_ERROR(status) || gop == NULL || gop->Mode == NULL || gop->Mode->Info == NULL) {
        PrintLn(L"GOP nije dostupan.");
        return;
    }

    DecodedImage image = { 0, 0, NULL };
    status = load_miimg_asset(SystemTable, asset_name, &image);
    if (status == EFI_NOT_FOUND) {
        Print(L"%s undefined\r\n", asset_name);
        return;
    }
    if (status == EFI_DEVICE_ERROR) {
        PrintLn(L"Ne mogu procitati .miimg datoteku.");
        return;
    }
    if (EFI_ERROR(status)) {
        PrintLn(L"Neispravan ili nepodrzan .miimg format.");
        return;
    }

    EFI_GRAPHICS_OUTPUT_BLT_PIXEL black = { 0x00, 0x00, 0x00, 0x00 };
    status = fill_screen(gop, black);
    if (!EFI_ERROR(status)) {
        status = draw_image_centered(gop, image.pixels, image.width, image.height);
    }

    free_decoded_image(SystemTable, &image);

    if (EFI_ERROR(status)) {
        PrintLn(L"Ne mogu prikazati sliku.");
        return;
    }

    wait_for_exit_key(SystemTable);
    show_terminal_home(SystemTable);
}

static void run_splash_asset(EFI_SYSTEM_TABLE *SystemTable, const CHAR16 *asset_name) {
    const CHAR16 *target_asset = asset_name;
    if (target_asset == NULL || target_asset[0] == L'\0') {
        target_asset = MIOS_SPLASH_ASSET;
    }

    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_STATUS status = get_gop(SystemTable, &gop);
    if (EFI_ERROR(status) || gop == NULL || gop->Mode == NULL || gop->Mode->Info == NULL) {
        PrintLn(L"GOP nije dostupan.");
        return;
    }

    DecodedImage image = { 0, 0, NULL };
    status = load_miimg_asset(SystemTable, target_asset, &image);
    if (!EFI_ERROR(status)) {
        status = run_image_splash(SystemTable, gop, &image);
        free_decoded_image(SystemTable, &image);
        if (EFI_ERROR(status)) {
            PrintLn(L"Ne mogu prikazati splash sliku.");
            return;
        }

        show_terminal_home(SystemTable);
        return;
    }

    if (status != EFI_NOT_FOUND) {
        if (status == EFI_DEVICE_ERROR) {
            PrintLn(L"Ne mogu procitati .miimg datoteku.");
        } else {
            PrintLn(L"Neispravan ili nepodrzan .miimg format.");
        }
        return;
    }

    show_loading_screen(SystemTable);

    MigifFile migif;
    MigifPlayerState player;
    status = load_migif_asset(SystemTable, gop, target_asset, &migif, &player);
    if (status == EFI_NOT_FOUND) {
        Print(L"%s undefined\r\n", target_asset);
        return;
    }
    if (status == EFI_DEVICE_ERROR) {
        PrintLn(L"Ne mogu procitati .migif datoteku.");
        return;
    }
    if (EFI_ERROR(status)) {
        PrintLn(L"Neispravan ili nepodrzan .migif format.");
        return;
    }

    status = run_video_splash(SystemTable, gop, &migif, &player);
    free_migif_player(SystemTable, &player);
    free_migif_file(SystemTable, &migif);

    if (EFI_ERROR(status)) {
        PrintLn(L"Ne mogu prikazati splash video.");
        return;
    }

    show_terminal_home(SystemTable);
}

static void run_play_asset(EFI_SYSTEM_TABLE *SystemTable, const CHAR16 *asset_name) {
    if (asset_name == NULL || asset_name[0] == L'\0') {
        PrintLn(L"Usage: play <asset>");
        return;
    }

    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_STATUS status = get_gop(SystemTable, &gop);
    if (EFI_ERROR(status) || gop == NULL || gop->Mode == NULL || gop->Mode->Info == NULL) {
        PrintLn(L"GOP nije dostupan.");
        return;
    }

    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *input_ex = NULL;
    status = get_text_input_ex(SystemTable, &input_ex);
    if (EFI_ERROR(status) || input_ex == NULL) {
        PrintLn(L"SimpleTextInputEx nije dostupan.");
        return;
    }

    show_loading_screen(SystemTable);

    MigifFile migif;
    MigifPlayerState player;
    status = load_migif_asset(SystemTable, gop, asset_name, &migif, &player);
    if (status == EFI_NOT_FOUND) {
        Print(L"%s undefined\r\n", asset_name);
        return;
    }
    if (status == EFI_DEVICE_ERROR) {
        PrintLn(L"Ne mogu procitati .migif datoteku.");
        return;
    }
    if (EFI_ERROR(status)) {
        PrintLn(L"Neispravan ili nepodrzan .migif format.");
        return;
    }

    EFI_GRAPHICS_OUTPUT_BLT_PIXEL black = { 0, 0, 0, 0 };
    status = fill_screen(gop, black);
    if (EFI_ERROR(status)) {
        free_migif_player(SystemTable, &player);
        free_migif_file(SystemTable, &migif);
        PrintLn(L"Ne mogu pripremiti ekran za video.");
        return;
    }

    status = render_migif_player(SystemTable, gop, &migif, &player);
    if (EFI_ERROR(status)) {
        free_migif_player(SystemTable, &player);
        free_migif_file(SystemTable, &migif);
        PrintLn(L"Ne mogu prikazati video.");
        return;
    }

    EFI_EVENT tick_event = NULL;
    status = uefi_call_wrapper(
        SystemTable->BootServices->CreateEvent,
        5,
        EVT_TIMER,
        TPL_APPLICATION,
        NULL,
        NULL,
        &tick_event
    );
    if (EFI_ERROR(status)) {
        free_migif_player(SystemTable, &player);
        free_migif_file(SystemTable, &migif);
        PrintLn(L"Ne mogu pokrenuti playback timer.");
        return;
    }

    status = arm_playback_timer(SystemTable, tick_event, &migif, &player);
    if (EFI_ERROR(status)) {
        uefi_call_wrapper(SystemTable->BootServices->CloseEvent, 1, tick_event);
        free_migif_player(SystemTable, &player);
        free_migif_file(SystemTable, &migif);
        PrintLn(L"Ne mogu pokrenuti playback timer.");
        return;
    }

    BOOLEAN exit_player = FALSE;
    while (!exit_player) {
        EFI_EVENT events[2];
        UINTN event_index = 0;
        events[0] = input_ex->WaitForKeyEx;
        events[1] = tick_event;

        status = uefi_call_wrapper(SystemTable->BootServices->WaitForEvent, 3, 2, events, &event_index);
        if (EFI_ERROR(status)) {
            break;
        }

        if (event_index == 1) {
            BOOLEAN render_needed = FALSE;
            status = advance_migif_playback(
                SystemTable,
                &migif,
                &player,
                migif_frame_remaining_100ns(&migif, &player),
                &render_needed
            );
            if (EFI_ERROR(status)) {
                break;
            }

            if (render_needed) {
                status = render_migif_player(SystemTable, gop, &migif, &player);
                if (EFI_ERROR(status)) {
                    break;
                }
            }

            status = arm_playback_timer(SystemTable, tick_event, &migif, &player);
            if (EFI_ERROR(status)) {
                break;
            }

            continue;
        }

        PlaybackInput input;
        zero_bytes(&input, sizeof(input));
        status = read_playback_input(input_ex, &input);
        if (status == EFI_NOT_READY) {
            continue;
        }
        if (EFI_ERROR(status)) {
            break;
        }

        BOOLEAN render_needed = FALSE;
        status = handle_playback_input(SystemTable, &migif, &player, &input, &exit_player, &render_needed);
        if (EFI_ERROR(status)) {
            break;
        }

        if (render_needed) {
            status = render_migif_player(SystemTable, gop, &migif, &player);
            if (EFI_ERROR(status)) {
                break;
            }
        }

        if (!exit_player) {
            status = arm_playback_timer(SystemTable, tick_event, &migif, &player);
            if (EFI_ERROR(status)) {
                break;
            }
        }
    }

    uefi_call_wrapper(SystemTable->BootServices->SetTimer, 3, tick_event, TimerCancel, 0);
    uefi_call_wrapper(SystemTable->BootServices->CloseEvent, 1, tick_event);
    free_migif_player(SystemTable, &player);
    free_migif_file(SystemTable, &migif);

    if (EFI_ERROR(status)) {
        PrintLn(L"Playback error.");
    }

    show_terminal_home(SystemTable);
}

static void handle_command(EFI_SYSTEM_TABLE *SystemTable, CHAR16 *input) {
    ParsedCommand command;
    parse_command(input, &command);

    if (command.name == NULL) {
        return;
    }

    if (is_command(&command, L"echo")) {
        for (UINTN i = 0; i < command.argc; i++) {
            if (i > 0) {
                Print(L" ");
            }
            Print(L"%s", command.argv[i]);
        }
        Print(L"\r\n");
        return;
    }

    if (is_command(&command, L"clear") || is_command(&command, L"cls")) {
        uefi_call_wrapper(SystemTable->ConOut->ClearScreen, 1, SystemTable->ConOut);
        return;
    }

    if (is_command(&command, L"draw")) {
        if (command.argc == 0) {
            PrintLn(L"Usage: draw <asset>");
            return;
        }

        run_draw_asset(SystemTable, command.argv[0]);
        return;
    }

    if (is_command(&command, L"play")) {
        if (command.argc == 0) {
            PrintLn(L"Usage: play <asset>");
            return;
        }

        run_play_asset(SystemTable, command.argv[0]);
        return;
    }

    if (is_command(&command, L"splash")) {
        run_splash_asset(SystemTable, command.argc == 0 ? NULL : command.argv[0]);
        return;
    }

    if (is_command(&command, L"shutdown")) {
        PrintLn(L"Turning off ...");
        uefi_call_wrapper(SystemTable->RuntimeServices->ResetSystem, 4, EfiResetShutdown, EFI_SUCCESS, 0, NULL);
        return;
    }

    if (is_command(&command, L"reboot")) {
        if (command.argc > 1 || (command.argc == 1 && str_cmp(command.argv[0], L"-c") != 0)) {
            PrintLn(L"Usage: reboot [-c]");
            return;
        }

        if (command.argc == 1) {
            PrintLn(L"Cold rebooting ...");
            uefi_call_wrapper(SystemTable->RuntimeServices->ResetSystem, 4, EfiResetCold, EFI_SUCCESS, 0, NULL);
            return;
        }

        PrintLn(L"Reloading grubx64.efi ...");
        EFI_STATUS status = reload_grubx64(SystemTable);
        if (EFI_ERROR(status)) {
            PrintLn(L"Ne mogu ponovo ucitati grubx64.efi.");
            show_terminal_home(SystemTable);
        }
        return;
    }

    if (is_command(&command, L"version")) {
        PrintLn(MIOS_VERSION);
        return;
    }

    if (is_command(&command, L"help")) {
        PrintLn(L"Commands:");
        PrintLn(L"  echo <text>");
        PrintLn(L"  clear");
        PrintLn(L"  draw <asset>");
        PrintLn(L"  play <asset>");
        PrintLn(L"  splash [asset]");
        PrintLn(L"  shutdown");
        PrintLn(L"  reboot [-c]");
        PrintLn(L"  version");
        PrintLn(L"  help");
        return;
    }

    Print(L"Unknown command: %s\r\n", command.name);
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    InitializeLib(ImageHandle, SystemTable);
    gImageHandle = ImageHandle;

    show_boot_splash(SystemTable);
    show_terminal_home(SystemTable);

    CHAR16 input[INPUT_BUFFER_SIZE];

    while (1) {
        print_prompt();
        read_line(SystemTable, input, INPUT_BUFFER_SIZE);
        handle_command(SystemTable, input);
    }

    return EFI_SUCCESS;
}
