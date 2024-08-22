#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "wlr-layer-shell-unstable-v1.h"

int alloc_shm_file(ptrdiff_t size) {
  const char shm_name[] = "/wlo-shm";
  int fd = ::shm_open(shm_name, O_RDWR | O_CREAT, 0600);
  if (fd < 0)
    return -1;

  ::shm_unlink(shm_name);
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
                zwlr_layer_surface_v1_ack_configure(surface, serial);
              },
          .closed =
              [](void *, zwlr_layer_surface_v1 *surface) {
                std::printf("%s\n", "layer_surface::closed");
                zwlr_layer_surface_v1_destroy(surface);
              },
      };

      zwlr_layer_surface_v1_set_size(overlay_surface, 100, 100);
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
    const int width = 100;
    const int height = 100;
    const int stride = width * 4;
    const int shm_size = stride * height * 2;
    int shm_fd = alloc_shm_file(shm_size);
    if (shm_fd < 0) {
      std::printf("failed to allocate shm file\n");
      return 1;
    }
    auto *pool_data = static_cast<uint8_t *>(
        mmap(nullptr, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0));

    auto *pool = wl_shm_create_pool(client_state.shm, shm_fd, shm_size);

    std::printf("pool: %p\n", (void *)pool);
    std::printf("data: %p\n", (void *)pool_data);

    int index = 0;
    int offset = height * stride * index;

    auto *buffer = wl_shm_pool_create_buffer(pool, offset, width, height,
                                             stride, WL_SHM_FORMAT_ARGB8888);

    ::memset(pool_data, 0x00, stride * height * 2);

    uint32_t *pixels = (uint32_t *)&pool_data[offset];
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        if ((x + y / 8 * 8) % 16 < 8) {
          pixels[y * width + x] = 0x08060606;
        } else {
          pixels[y * width + x] = 0x08080808;
        }
      }
    }

    wl_surface_attach(client_state.layer_surface.surface, buffer, 0, 0);
    wl_surface_damage(client_state.layer_surface.surface, 0, 0, width, height);
    wl_surface_commit(client_state.layer_surface.surface);
    wl_display_flush(display);
    ::getchar();
  }

  wl_registry_destroy(registry);
  wl_display_disconnect(display);
}
