#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <unistd.h>

#include "wlr-layer-shell-unstable-v1.h"

#include <ft2build.h>
#include FT_FREETYPE_H

FT_Library g_ft_library;
FT_Face g_ft_face;

inline int init_ft() {
  FT_Error error = FT_Init_FreeType(&g_ft_library);
  if (error != 0) {
    std::printf("failed to init freetype\n");
    return 1;
  }

  error = FT_New_Face(g_ft_library,
                      "/usr/share/fonts/sarasa-gothic/Sarasa-Regular.ttc", 20,
                      &g_ft_face);

  error = FT_Set_Pixel_Sizes(g_ft_face, 0, 16);

  if (error != 0) {
    std::printf("failed to load font face\n");
    return 1;
  }

  return 0;
}

inline int render_mono_argb(uint32_t *out_buf, int buf_width, int buf_height,
                            char code, int anchor_x, int anchor_y,
                            uint8_t alpha = 0xff) {
  auto error = FT_Load_Char(g_ft_face, code, FT_LOAD_RENDER);
  if (error != 0) {
    std::printf("failed to load glyph\n");
    return 1;
  }

  auto *glyph = g_ft_face->glyph;
  for (int r = 0; r < glyph->bitmap.rows; ++r) {
    for (int c = 0; c < glyph->bitmap.width; ++c) {
      const int buf_x = c + anchor_x + glyph->bitmap_left;
      const int buf_y = r + anchor_y - glyph->bitmap_top;
      auto *p = &out_buf[buf_y * buf_width + buf_x];
      const uint32_t value =
          static_cast<uint32_t>(
              glyph->bitmap.buffer[r * glyph->bitmap.width + c]) *
          alpha / 256U;
      *p = value | (value << 8U) | (value << 16U) | (value << 24U);
    }
  }

  return 0;
}

inline int render_line(uint32_t *out_buf, int buf_width, int buf_height,
                       const char *line, size_t line_size, int anchor_y,
                       uint8_t alpha = 0xff) {
  int x = 0;
  for (const char *p = line; p != line + line_size; ++p) {
    render_mono_argb(out_buf, buf_width, buf_height, *p, x, anchor_y, alpha);
    x += 10;
    if (x >= buf_width)
      break;
  }
  return 0;
}

int alloc_shm_file(ptrdiff_t size) {
  int fd = ::memfd_create("wlo-shm", MFD_CLOEXEC | MFD_ALLOW_SEALING);

  if (fd < 0)
    return -1;

  if (::ftruncate(fd, size) < 0) {
    ::close(fd);
    return -1;
  }

  return fd;
}

int main() {

  std::printf("pid: %d\n", ::getpid());

  struct ly_surface_t {
    wl_output *output = nullptr;
    wl_surface *surface = nullptr;
    zwlr_layer_surface_v1 *layer_surface = nullptr;
  };

  struct client_state_t {
    wl_compositor *compositor = nullptr;
    wl_shm *shm = nullptr;
    std::vector<wl_output *> outputs{};
    zwlr_layer_shell_v1 *layer_shell = nullptr;

    ly_surface_t layer_surface;

    [[nodiscard]] ly_surface_t output_add_surface(wl_output *output) const {

      ly_surface_t out;
      out.output = output;

      auto *surf = wl_compositor_create_surface(compositor);

      auto *input_region = wl_compositor_create_region(compositor);
      wl_surface_set_input_region(surf, input_region);
      wl_region_destroy(input_region);

      auto *overlay_surface = zwlr_layer_shell_v1_get_layer_surface(
          layer_shell, surf, output, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
          "overlay");

      out.surface = surf;
      out.layer_surface = overlay_surface;

      static zwlr_layer_surface_v1_listener layer_surface_listener = {
          .configure =
              [](void *, zwlr_layer_surface_v1 *surface, uint32_t serial,
                 uint32_t width, uint32_t height) {
                std::printf("%s: %d %d %d\n", "layer_surface::configure",
                            serial, width, height);
                if (width != 200 || height != 200) {
                  std::printf("unexpected width or height\n");
                  return;
                }
                zwlr_layer_surface_v1_ack_configure(surface, serial);
              },
          .closed =
              [](void *, zwlr_layer_surface_v1 *surface) {
                std::printf("%s\n", "layer_surface::closed");
                zwlr_layer_surface_v1_destroy(surface);
              },
      };

      zwlr_layer_surface_v1_set_anchor(overlay_surface,
                                       ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                           ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
      zwlr_layer_surface_v1_set_size(overlay_surface, 200, 200);
      zwlr_layer_surface_v1_add_listener(overlay_surface,
                                         &layer_surface_listener, nullptr);
      wl_surface_commit(surf);
      std::printf("committed %p\n", (void *)surf);
      return out;
    }
  } client_state;

  auto *display = wl_display_connect(nullptr);

  auto *registry = wl_display_get_registry(display);
  {
    static wl_registry_listener registry_listener = {
        .global =
            [](void *data, wl_registry *registry, uint32_t name,
               const char *interface, uint32_t version [[maybe_unused]]) {
              std::printf("%s: %d %s\n", "registry::global", name, interface);

              auto *state = static_cast<client_state_t *>(data);

#define INTERFACE_MATCH(interface_name)                                        \
  if (std::strcmp(interface, interface_name) == 0)

              INTERFACE_MATCH(wl_compositor_interface.name) {
                state->compositor =
                    static_cast<wl_compositor *>(wl_registry_bind(
                        registry, name, &wl_compositor_interface, 4));
                std::printf("got wl_compositor: %p\n",
                            static_cast<void *>(state->compositor));
                return;
              }

              INTERFACE_MATCH(wl_shm_interface.name) {
                state->shm = static_cast<wl_shm *>(
                    wl_registry_bind(registry, name, &wl_shm_interface, 1));
                std::printf("got wl_shm: %p\n",
                            static_cast<void *>(state->shm));
                return;
              }

              INTERFACE_MATCH(wl_output_interface.name) {
                auto *output = static_cast<wl_output *>(
                    wl_registry_bind(registry, name, &wl_output_interface, 3));
                state->outputs.push_back(output);
                std::printf("got wl_output: %p\n", static_cast<void *>(output));
                return;
              }

              INTERFACE_MATCH(zwlr_layer_shell_v1_interface.name) {
                state->layer_shell =
                    static_cast<zwlr_layer_shell_v1 *>(wl_registry_bind(
                        registry, name, &zwlr_layer_shell_v1_interface, 2));
                std::printf("got zwlr_layer_shell_v1: %p\n",
                            static_cast<void *>(state->layer_shell));
                return;
              }

#undef INTERFACE_MATCH
            },

        .global_remove =
            [](void *, wl_registry *, uint32_t name) {
              std::printf("%s: %d\n", "registry::global_remove", name);
            },
    };

    wl_registry_add_listener(registry, &registry_listener, &client_state);
  }

  wl_display_roundtrip(display);
  client_state.layer_surface =
      client_state.output_add_surface(client_state.outputs[0]);
  wl_display_roundtrip(display);

  {
    const int width = 200;
    const int height = 200;
    const int stride = width * 4;
    const int shm_size = stride * height * 2;
    int shm_fd = alloc_shm_file(shm_size);
    if (shm_fd < 0) {
      std::printf("failed to allocate shm file\n");
      return 1;
    }
    auto *pool_data = static_cast<uint8_t *>(::mmap(
        nullptr, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0));

    auto *pool = wl_shm_create_pool(client_state.shm, shm_fd, shm_size);

    wl_buffer *pbufs[2];

    for (int i = 0; i < 2; ++i)
      pbufs[i] =
          wl_shm_pool_create_buffer(pool, height * stride * i, width, height,
                                    stride, WL_SHM_FORMAT_ARGB8888);

    if (init_ft() != 0)
      return 1;

    int back_index = 0;
    int anchor_y = 20;
    wl_surface *surface = client_state.layer_surface.surface;

    char *linebuf = nullptr;
    size_t linebuf_size = 0;

    for (;;) {
      const int offset = height * stride * back_index;
      const int buf_size = height * stride;
      wl_buffer *pbuf = pbufs[back_index];
      uint8_t *buf_data = &pool_data[offset];
      uint32_t *pixels = (uint32_t *)buf_data;

      std::string text = "A now";
      text[0] = 'A' + back_index;

      bool scrolled = false;
      anchor_y += 16;
      if (anchor_y >= height - 16) {
        anchor_y = 20;
        scrolled = true;
        std::printf("scrolled\n");
      }

      const ssize_t read_cnt = ::getline(&linebuf, &linebuf_size, stdin);
      if (read_cnt < 0) {
        std::printf("failed to read line, %s\n", strerror(errno));
        return -1;
      }

      ::memset(buf_data, 0x00, buf_size);
      render_line(pixels, width, height, linebuf, read_cnt, anchor_y, 0x10);
      const int damaged_x = 0;
      const int damaged_y = anchor_y - 16;
      const int damaged_width = width;
      const int damaged_height = 2 * 16;

      wl_surface_attach(surface, pbuf, 0, 0);
      wl_surface_damage_buffer(surface, damaged_x, damaged_y, damaged_width,
                               damaged_height);
      if (scrolled)
        wl_surface_damage_buffer(surface, 0, 0, width, height);
      wl_surface_commit(surface);
      while (wl_display_flush(display) < 0) {
        if (errno != EAGAIN) {
          std::printf("flush failed: %s\n", strerror(errno));
          return -1;
        }
        struct pollfd fds[] = {
            {.fd = wl_display_get_fd(display), .events = POLLIN},
        };
        int ret = ::poll(fds, 1, -1);
        if (ret < 0) {
          if (errno == EINTR)
            continue;
          std::printf("failed to poll\n");
        }
      }
      back_index = 1 - back_index;
      // TODO: libwayland: data too big for buffer if we flush too frequently
    }
    ::free(linebuf);
  }

  wl_registry_destroy(registry);
  wl_display_disconnect(display);
}
