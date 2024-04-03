#include <wayfire/plugin.hpp>
#include <wayfire/per-output-plugin.hpp>
#include <wayfire/view.hpp>
#include <wayfire/output.hpp>
#include <wayfire/toplevel-view.hpp>
#include <wayfire/seat.hpp>
#include <wayfire/input-device.hpp>
#include <set>

#include "plugins/ipc/ipc-helpers.hpp"
#include "plugins/ipc/ipc-method-repository.hpp"
#include "wayfire/core.hpp"
#include "wayfire/plugins/common/util.hpp"
#include "wayfire/unstable/wlr-surface-node.hpp"
#include "wayfire/plugins/common/shared-core-data.hpp"
#include "wayfire/signal-definitions.hpp"
#include "wayfire/signal-provider.hpp"
#include "wayfire/view-helpers.hpp"
#include "wayfire/window-manager.hpp"
#include "wayfire/workarea.hpp"
#include "config.h"
#include <wayfire/debug.hpp>
#include <wayfire/nonstd/wlroots-full.hpp>


static std::string role_to_string(enum wf::view_role_t role)
{
    switch (role)
    {
      case wf::VIEW_ROLE_TOPLEVEL:
        return "toplevel";

      case wf::VIEW_ROLE_UNMANAGED:
        return "unmanaged";

      case wf::VIEW_ROLE_DESKTOP_ENVIRONMENT:
        return "desktop-environment";

      default:
        return "unknown";
    }
}

static std::string layer_to_string(std::optional<wf::scene::layer> layer)
{
    if (!layer.has_value())
    {
        return "none";
    }

    switch (layer.value())
    {
      case wf::scene::layer::BACKGROUND:
        return "background";

      case wf::scene::layer::BOTTOM:
        return "bottom";

      case wf::scene::layer::WORKSPACE:
        return "workspace";

      case wf::scene::layer::TOP:
        return "top";

      case wf::scene::layer::UNMANAGED:
        return "unmanaged";

      case wf::scene::layer::OVERLAY:
        return "overlay";

      case wf::scene::layer::LOCK:
        return "lock";

      case wf::scene::layer::DWIDGET:
        return "dew";

      default:
        break;
    }

    wf::dassert(false, "invalid layer!");
    assert(false); // prevent compiler warning
}

static std::string wlr_input_device_type_to_string(wlr_input_device_type type)
{
    switch (type)
    {
      case WLR_INPUT_DEVICE_KEYBOARD:
        return "keyboard";

      case WLR_INPUT_DEVICE_POINTER:
        return "pointer";

      case WLR_INPUT_DEVICE_TOUCH:
        return "touch";

      case WLR_INPUT_DEVICE_TABLET_TOOL:
        return "tablet_tool";

      case WLR_INPUT_DEVICE_TABLET_PAD:
        return "tablet_pad";

      case WLR_INPUT_DEVICE_SWITCH:
        return "switch";

      default:
        return "unknown";
    }
}

static wf::geometry_t get_view_base_geometry(wayfire_view view)
{
    auto sroot = view->get_surface_root_node();
    for (auto& ch : sroot->get_children())
    {
        if (auto wlr_surf = dynamic_cast<wf::scene::wlr_surface_node_t*>(ch.get()))
        {
            auto bbox = wlr_surf->get_bounding_box();
            wf::pointf_t origin = sroot->to_global({0, 0});
            bbox.x = origin.x;
            bbox.y = origin.y;
            return bbox;
        }
    }

    return sroot->get_bounding_box();
}

class ipc_rules_t : public wf::plugin_interface_t, public wf::per_output_tracker_mixin_t<>
{
  public:
    void init() override
    {
        method_repository->register_method("wayfire/configuration", get_wayfire_configuration_info);
        method_repository->register_method("input/list-devices", list_input_devices);
        method_repository->register_method("input/configure-device", configure_input_device);
        method_repository->register_method("window-rules/events/watch", on_client_watch);
        method_repository->register_method("window-rules/list-views", list_views);
        method_repository->register_method("window-rules/list-outputs", list_outputs);
        method_repository->register_method("window-rules/list-wsets", list_wsets);
        method_repository->register_method("window-rules/view-info", get_view_info);
        method_repository->register_method("window-rules/output-info", get_output_info);
        method_repository->register_method("window-rules/wset-info", get_wset_info);
        method_repository->register_method("window-rules/configure-view", configure_view);
        method_repository->register_method("window-rules/focus-view", focus_view);
        method_repository->register_method("window-rules/get-focused-view", get_focused_view);
        method_repository->register_method("window-rules/get-focused-output", get_focused_output);
        method_repository->register_method("window-rules/close-view", close_view);
        method_repository->connect(&on_client_disconnected);
        init_output_tracking();
    }

    void fini() override
    {
        method_repository->unregister_method("wayfire/configuration");
        method_repository->unregister_method("input/list-devices");
        method_repository->unregister_method("input/configure-device");
        method_repository->unregister_method("window-rules/events/watch");
        method_repository->unregister_method("window-rules/list-views");
        method_repository->unregister_method("window-rules/list-outputs");
        method_repository->unregister_method("window-rules/list-wsets");
        method_repository->unregister_method("window-rules/view-info");
        method_repository->unregister_method("window-rules/output-info");
        method_repository->unregister_method("window-rules/wset-info");
        method_repository->unregister_method("window-rules/configure-view");
        method_repository->unregister_method("window-rules/focus-view");
        method_repository->unregister_method("window-rules/get-focused-view");
        method_repository->unregister_method("window-rules/get-focused-output");
        method_repository->unregister_method("window-rules/close-view");
        fini_output_tracking();
    }

    void handle_new_output(wf::output_t *output) override
    {
        for (auto& [_, event] : signal_map)
        {
            if (event.connected_count)
            {
                event.register_output(output);
            }
        }

        nlohmann::json data;
        data["event"]  = "output-added";
        data["output"] = output_to_json(output);
        send_event_to_subscribes(data, data["event"]);
    }

    void handle_output_removed(wf::output_t *output) override
    {
        nlohmann::json data;
        data["event"]  = "output-removed";
        data["output"] = output_to_json(output);
        send_event_to_subscribes(data, data["event"]);
    }

    // Template FOO for efficient management of signals: ensure that only actually listened-for signals
    // are connected.
    struct signal_registration_handler
    {
        std::function<void()> register_core = [] () {};
        std::function<void(wf::output_t*)> register_output = [] (wf::output_t*) {};
        std::function<void()> unregister = [] () {};
        int connected_count = 0;

        void increase_count()
        {
            connected_count++;
            if (connected_count > 1)
            {
                return;
            }

            register_core();
            for (auto& wo : wf::get_core().output_layout->get_outputs())
            {
                register_output(wo);
            }
        }

        void decrease_count()
        {
            connected_count--;
            if (connected_count > 0)
            {
                return;
            }

            unregister();
        }
    };

    template<class Signal>
    static signal_registration_handler get_generic_core_registration_cb(
        wf::signal::connection_t<Signal> *conn)
    {
        return {
            .register_core = [=] () { wf::get_core().connect(conn); },
            .unregister    = [=] () { conn->disconnect(); }
        };
    }

    template<class Signal>
    signal_registration_handler get_generic_output_registration_cb(wf::signal::connection_t<Signal> *conn)
    {
        return {
            .register_output = [=] (wf::output_t *wo) { wo->connect(conn); },
            .unregister = [=] () { conn->disconnect(); }
        };
    }

    std::map<std::string, signal_registration_handler> signal_map =
    {
        {"view-mapped", get_generic_core_registration_cb(&on_view_mapped)},
        {"view-unmapped", get_generic_core_registration_cb(&on_view_unmapped)},
        {"view-set-output", get_generic_core_registration_cb(&on_view_set_output)},
        {"view-geometry-changed", get_generic_core_registration_cb(&on_view_geometry_changed)},
        {"view-wset-changed", get_generic_core_registration_cb(&on_view_moved_to_wset)},
        {"view-focused", get_generic_core_registration_cb(&on_kbfocus_changed)},
        {"view-title-changed", get_generic_core_registration_cb(&on_title_changed)},
        {"view-app-id-changed", get_generic_core_registration_cb(&on_app_id_changed)},
        {"plugin-activation-state-changed", get_generic_core_registration_cb(&on_plugin_activation_changed)},
        {"output-gain-focus", get_generic_core_registration_cb(&on_output_gain_focus)},

        {"view-tiled", get_generic_output_registration_cb(&_tiled)},
        {"view-minimized", get_generic_output_registration_cb(&_minimized)},
        {"view-fullscreened", get_generic_output_registration_cb(&_fullscreened)},
        {"view-sticky", get_generic_output_registration_cb(&_stickied)},
        {"view-workspace-changed", get_generic_output_registration_cb(&_view_workspace)},
        {"output-wset-changed", get_generic_output_registration_cb(&on_wset_changed)},
        {"wset-workspace-changed", get_generic_output_registration_cb(&on_wset_workspace_changed)},
    };

    wf::ipc::method_callback get_wayfire_configuration_info = [=] (nlohmann::json)
    {
        nlohmann::json response;

        response["api-version"]    = WAYFIRE_API_ABI_VERSION;
        response["plugin-path"]    = PLUGIN_PATH;
        response["plugin-xml-dir"] = PLUGIN_XML_DIR;
        response["xwayland-support"] = WF_HAS_XWAYLAND;

        response["build-commit"] = wf::version::git_commit;
        response["build-branch"] = wf::version::git_branch;
        return response;
    };

    wf::ipc::method_callback list_views = [=] (nlohmann::json)
    {
        auto response = nlohmann::json::array();

        for (auto& view : wf::get_core().get_all_views())
        {
            nlohmann::json v = view_to_json(view);
            response.push_back(v);
        }

        return response;
    };

    wf::ipc::method_callback get_view_info = [=] (nlohmann::json data)
    {
        WFJSON_EXPECT_FIELD(data, "id", number_integer);
        if (auto view = wf::ipc::find_view_by_id(data["id"]))
        {
            auto response = wf::ipc::json_ok();
            response["info"] = view_to_json(view);
            return response;
        }

        return wf::ipc::json_error("no such view");
    };

    wf::ipc::method_callback get_focused_view = [=] (nlohmann::json data)
    {
        if (auto view = wf::get_core().seat->get_active_view())
        {
            auto response = wf::ipc::json_ok();
            response["info"] = view_to_json(view);
            return response;
        } else
        {
            auto response = wf::ipc::json_ok();
            response["info"] = nullptr;
            return response;
        }
    };

    wf::ipc::method_callback get_focused_output = [=] (nlohmann::json data)
    {
        auto active_output = wf::get_core().seat->get_active_output();
        auto response = wf::ipc::json_ok();

        if (active_output)
        {
            response["info"] = output_to_json(active_output);
        } else
        {
            response["info"] = nullptr;
        }

        return response;
    };

    wf::ipc::method_callback focus_view = [=] (nlohmann::json data)
    {
        WFJSON_EXPECT_FIELD(data, "id", number_integer);
        if (auto view = wf::ipc::find_view_by_id(data["id"]))
        {
            auto response = wf::ipc::json_ok();
            auto toplevel = wf::toplevel_cast(view);
            if (!toplevel)
            {
                return wf::ipc::json_error("view is not toplevel");
            }

            wf::get_core().default_wm->focus_request(toplevel);
            return response;
        }

        return wf::ipc::json_error("no such view");
    };

    wf::ipc::method_callback close_view = [=] (nlohmann::json data)
    {
        WFJSON_EXPECT_FIELD(data, "id", number_integer);
        if (auto view = wf::ipc::find_view_by_id(data["id"]))
        {
            auto response = wf::ipc::json_ok();
            view->close();
            return response;
        }

        return wf::ipc::json_error("no such view");
    };

    nlohmann::json output_to_json(wf::output_t *o)
    {
        if (!o)
        {
            return nullptr;
        }

        nlohmann::json response;
        response["id"]   = o->get_id();
        response["name"] = o->to_string();
        response["geometry"]   = wf::ipc::geometry_to_json(o->get_layout_geometry());
        response["workarea"]   = wf::ipc::geometry_to_json(o->workarea->get_workarea());
        response["wset-index"] = o->wset()->get_index();
        response["workspace"]["x"] = o->wset()->get_current_workspace().x;
        response["workspace"]["y"] = o->wset()->get_current_workspace().y;
        response["workspace"]["grid_width"]  = o->wset()->get_workspace_grid_size().width;
        response["workspace"]["grid_height"] = o->wset()->get_workspace_grid_size().height;
        return response;
    }

    wf::ipc::method_callback list_outputs = [=] (nlohmann::json)
    {
        auto response = nlohmann::json::array();
        for (auto& output : wf::get_core().output_layout->get_outputs())
        {
            response.push_back(output_to_json(output));
        }

        return response;
    };

    wf::ipc::method_callback get_output_info = [=] (nlohmann::json data)
    {
        WFJSON_EXPECT_FIELD(data, "id", number_integer);
        auto wo = wf::ipc::find_output_by_id(data["id"]);
        if (!wo)
        {
            return wf::ipc::json_error("output not found");
        }

        auto response = output_to_json(wo);
        return response;
    };

    wf::ipc::method_callback configure_view = [=] (nlohmann::json data)
    {
        WFJSON_EXPECT_FIELD(data, "id", number_integer);
        WFJSON_OPTIONAL_FIELD(data, "output_id", number_integer);
        WFJSON_OPTIONAL_FIELD(data, "geometry", object);
        WFJSON_OPTIONAL_FIELD(data, "sticky", boolean);

        auto view = wf::ipc::find_view_by_id(data["id"]);
        if (!view)
        {
            return wf::ipc::json_error("view not found");
        }

        auto toplevel = wf::toplevel_cast(view);
        if (!toplevel)
        {
            return wf::ipc::json_error("view is not toplevel");
        }

        if (data.contains("output_id"))
        {
            auto wo = wf::ipc::find_output_by_id(data["output_id"]);
            if (!wo)
            {
                return wf::ipc::json_error("output not found");
            }

            wf::move_view_to_output(toplevel, wo, !data.contains("geometry"));
        }

        if (data.contains("geometry"))
        {
            auto geometry = wf::ipc::geometry_from_json(data["geometry"]);
            if (!geometry)
            {
                return wf::ipc::json_error("invalid geometry");
            }

            toplevel->set_geometry(*geometry);
        }

        if (data.contains("sticky"))
        {
            toplevel->set_sticky(data["sticky"]);
        }

        return wf::ipc::json_ok();
    };

    nlohmann::json wset_to_json(wf::workspace_set_t *wset)
    {
        if (!wset)
        {
            return nullptr;
        }

        nlohmann::json response;
        response["index"] = wset->get_index();
        response["name"]  = wset->to_string();

        auto output = wset->get_attached_output();
        response["output-id"]   = output ? (int)output->get_id() : -1;
        response["output-name"] = output ? output->to_string() : "";
        response["workspace"]["x"] = wset->get_current_workspace().x;
        response["workspace"]["y"] = wset->get_current_workspace().y;
        response["workspace"]["grid_width"]  = wset->get_workspace_grid_size().width;
        response["workspace"]["grid_height"] = wset->get_workspace_grid_size().height;
        return response;
    }

    wf::ipc::method_callback list_wsets = [=] (nlohmann::json)
    {
        auto response = nlohmann::json::array();
        for (auto& workspace_set : wf::workspace_set_t::get_all())
        {
            response.push_back(wset_to_json(workspace_set.get()));
        }

        return response;
    };

    wf::ipc::method_callback get_wset_info = [=] (nlohmann::json data)
    {
        WFJSON_EXPECT_FIELD(data, "id", number_integer);
        auto ws = wf::ipc::find_workspace_set_by_index(data["id"]);
        if (!ws)
        {
            return wf::ipc::json_error("workspace set not found");
        }

        auto response = wset_to_json(ws);
        return response;
    };

  private:
    wf::shared_data::ref_ptr_t<wf::ipc::method_repository_t> method_repository;

    // Track a list of clients which have requested watch
    std::map<wf::ipc::client_interface_t*, std::set<std::string>> clients;

    wf::ipc::method_callback_full on_client_watch =
        [=] (nlohmann::json data, wf::ipc::client_interface_t *client)
    {
        static constexpr const char *EVENTS = "events";
        WFJSON_OPTIONAL_FIELD(data, EVENTS, array);
        std::set<std::string> subscribed_to;
        if (data.contains(EVENTS))
        {
            for (auto& sub : data[EVENTS])
            {
                if (!sub.is_string())
                {
                    return wf::ipc::json_error("Event list contains non-string entries!");
                }

                if (signal_map.count(sub))
                {
                    subscribed_to.insert((std::string)sub);
                }
            }
        } else
        {
            for (auto& [ev_name, _] : signal_map)
            {
                subscribed_to.insert(ev_name);
            }
        }

        for (auto& ev_name : subscribed_to)
        {
            signal_map[ev_name].increase_count();
        }

        clients[client] = subscribed_to;
        return wf::ipc::json_ok();
    };

    wf::signal::connection_t<wf::ipc::client_disconnected_signal> on_client_disconnected =
        [=] (wf::ipc::client_disconnected_signal *ev)
    {
        for (auto& ev_name : clients[ev->client])
        {
            signal_map[ev_name].decrease_count();
        }

        clients.erase(ev->client);
    };

    void send_view_to_subscribes(wayfire_view view, std::string event_name)
    {
        nlohmann::json event;
        event["event"] = event_name;
        event["view"]  = view_to_json(view);
        send_event_to_subscribes(event, event_name);
    }

    void send_event_to_subscribes(const nlohmann::json& data, const std::string& event_name)
    {
        for (auto& [client, events] : clients)
        {
            if (events.empty() || events.count(event_name))
            {
                client->send_json(data);
            }
        }
    }

    wf::signal::connection_t<wf::view_mapped_signal> on_view_mapped = [=] (wf::view_mapped_signal *ev)
    {
        send_view_to_subscribes(ev->view, "view-mapped");
    };

    wf::signal::connection_t<wf::view_unmapped_signal> on_view_unmapped = [=] (wf::view_unmapped_signal *ev)
    {
        send_view_to_subscribes(ev->view, "view-unmapped");
    };

    wf::signal::connection_t<wf::view_set_output_signal> on_view_set_output =
        [=] (wf::view_set_output_signal *ev)
    {
        nlohmann::json data;
        data["event"]  = "view-set-output";
        data["output"] = output_to_json(ev->output);
        data["view"]   = view_to_json(ev->view);
        send_event_to_subscribes(data, data["event"]);
    };

    wf::signal::connection_t<wf::view_geometry_changed_signal> on_view_geometry_changed =
        [=] (wf::view_geometry_changed_signal *ev)
    {
        nlohmann::json data;
        data["event"] = "view-geometry-changed";
        data["old-geometry"] = wf::ipc::geometry_to_json(ev->old_geometry);
        data["view"] = view_to_json(ev->view);
        send_event_to_subscribes(data, data["event"]);
    };

    wf::signal::connection_t<wf::view_moved_to_wset_signal> on_view_moved_to_wset =
        [=] (wf::view_moved_to_wset_signal *ev)
    {
        nlohmann::json data;
        data["event"]    = "view-wset-changed";
        data["old-wset"] = wset_to_json(ev->old_wset.get());
        data["new-wset"] = wset_to_json(ev->new_wset.get());
        data["view"]     = view_to_json(ev->view);
        send_event_to_subscribes(data, data["event"]);
    };

    wf::signal::connection_t<wf::keyboard_focus_changed_signal> on_kbfocus_changed =
        [=] (wf::keyboard_focus_changed_signal *ev)
    {
        send_view_to_subscribes(wf::node_to_view(ev->new_focus), "view-focused");
    };

    // Tiled rule handler.
    wf::signal::connection_t<wf::view_tiled_signal> _tiled = [=] (wf::view_tiled_signal *ev)
    {
        nlohmann::json data;
        data["event"]     = "view-tiled";
        data["old-edges"] = ev->old_edges;
        data["new-edges"] = ev->new_edges;
        data["view"] = view_to_json(ev->view);
        send_event_to_subscribes(data, data["event"]);
    };

    // Minimized rule handler.
    wf::signal::connection_t<wf::view_minimized_signal> _minimized = [=] (wf::view_minimized_signal *ev)
    {
        send_view_to_subscribes(ev->view, "view-minimized");
    };

    // Fullscreened rule handler.
    wf::signal::connection_t<wf::view_fullscreen_signal> _fullscreened = [=] (wf::view_fullscreen_signal *ev)
    {
        send_view_to_subscribes(ev->view, "view-fullscreen");
    };

    // Stickied rule handler.
    wf::signal::connection_t<wf::view_set_sticky_signal> _stickied = [=] (wf::view_set_sticky_signal *ev)
    {
        send_view_to_subscribes(ev->view, "view-sticky");
    };

    wf::signal::connection_t<wf::view_change_workspace_signal> _view_workspace =
        [=] (wf::view_change_workspace_signal *ev)
    {
        nlohmann::json data;
        data["event"] = "view-workspace-changed";
        data["from"]  = wf::ipc::point_to_json(ev->from);
        data["to"]    = wf::ipc::point_to_json(ev->to);
        data["view"]  = view_to_json(ev->view);
        send_event_to_subscribes(data, data["event"]);
    };

    wf::signal::connection_t<wf::view_title_changed_signal> on_title_changed =
        [=] (wf::view_title_changed_signal *ev)
    {
        send_view_to_subscribes(ev->view, "view-title-changed");
    };

    wf::signal::connection_t<wf::view_app_id_changed_signal> on_app_id_changed =
        [=] (wf::view_app_id_changed_signal *ev)
    {
        send_view_to_subscribes(ev->view, "view-app-id-changed");
    };

    wf::signal::connection_t<wf::output_plugin_activated_changed_signal> on_plugin_activation_changed =
        [=] (wf::output_plugin_activated_changed_signal *ev)
    {
        nlohmann::json data;
        data["event"]  = "plugin-activation-state-changed";
        data["plugin"] = ev->plugin_name;
        data["state"]  = ev->activated;
        data["output"] = ev->output ? (int)ev->output->get_id() : -1;
        data["output-data"] = output_to_json(ev->output);
        send_event_to_subscribes(data, data["event"]);
    };

    wf::signal::connection_t<wf::output_gain_focus_signal> on_output_gain_focus =
        [=] (wf::output_gain_focus_signal *ev)
    {
        nlohmann::json data;
        data["event"]  = "output-gain-focus";
        data["output"] = output_to_json(ev->output);
        send_event_to_subscribes(data, data["event"]);
    };

    wf::signal::connection_t<wf::workspace_set_changed_signal> on_wset_changed =
        [=] (wf::workspace_set_changed_signal *ev)
    {
        nlohmann::json data;
        data["event"]    = "output-wset-changed";
        data["new-wset"] = ev->new_wset ? (int)ev->new_wset->get_id() : -1;
        data["output"]   = ev->output ? (int)ev->output->get_id() : -1;
        data["new-wset-data"] = wset_to_json(ev->new_wset.get());
        data["output-data"]   = output_to_json(ev->output);
        send_event_to_subscribes(data, data["event"]);
    };

    wf::signal::connection_t<wf::workspace_changed_signal> on_wset_workspace_changed =
        [=] (wf::workspace_changed_signal *ev)
    {
        nlohmann::json data;
        data["event"] = "wset-workspace-changed";
        data["previous-workspace"] = wf::ipc::point_to_json(ev->old_viewport);
        data["new-workspace"] = wf::ipc::point_to_json(ev->new_viewport);
        data["output"] = ev->output ? (int)ev->output->get_id() : -1;
        data["wset"]   = (ev->output && ev->output->wset()) ? (int)ev->output->wset()->get_id() : -1;
        data["output-data"] = output_to_json(ev->output);
        data["wset-data"]   = ev->output ? wset_to_json(ev->output->wset().get()) : nullptr;
        send_event_to_subscribes(data, data["event"]);
    };

    std::string get_view_type(wayfire_view view)
    {
        if (view->role == wf::VIEW_ROLE_TOPLEVEL)
        {
            return "toplevel";
        }

        if (view->role == wf::VIEW_ROLE_UNMANAGED)
        {
#if WF_HAS_XWAYLAND
            auto surf = view->get_wlr_surface();
            if (surf && wlr_xwayland_surface_try_from_wlr_surface(surf))
            {
                return "x-or";
            }

#endif

            return "unmanaged";
        }

        auto layer = wf::get_view_layer(view);
        if ((layer == wf::scene::layer::BACKGROUND) || (layer == wf::scene::layer::BOTTOM))
        {
            return "background";
        } else if (layer == wf::scene::layer::TOP)
        {
            return "panel";
        } else if (layer == wf::scene::layer::OVERLAY)
        {
            return "overlay";
        }

        return "unknown";
    }

    nlohmann::json view_to_json(wayfire_view view)
    {
        if (!view)
        {
            return nullptr;
        }

        auto output = view->get_output();
        nlohmann::json description;
        description["id"]     = view->get_id();
        description["pid"]    = get_view_pid(view);
        description["title"]  = view->get_title();
        description["app-id"] = view->get_app_id();
        description["base-geometry"] = wf::ipc::geometry_to_json(get_view_base_geometry(view));
        auto toplevel = wf::toplevel_cast(view);
        description["parent"]   = toplevel && toplevel->parent ? (int)toplevel->parent->get_id() : -1;
        description["geometry"] =
            wf::ipc::geometry_to_json(toplevel ? toplevel->get_pending_geometry() : view->get_bounding_box());
        description["bbox"] = wf::ipc::geometry_to_json(view->get_bounding_box());
        description["output-id"]   = view->get_output() ? view->get_output()->get_id() : -1;
        description["output-name"] = output ? output->to_string() : "null";
        description["last-focus-timestamp"] = wf::get_focus_timestamp(view);
        description["role"]   = role_to_string(view->role);
        description["mapped"] = view->is_mapped();
        description["layer"]  = layer_to_string(get_view_layer(view));
        description["tiled-edges"] = toplevel ? toplevel->pending_tiled_edges() : 0;
        description["fullscreen"]  = toplevel ? toplevel->pending_fullscreen() : false;
        description["minimized"]   = toplevel ? toplevel->minimized : false;
        description["activated"]   = toplevel ? toplevel->activated : false;
        description["sticky"]     = toplevel ? toplevel->sticky : false;
        description["wset-index"] = toplevel && toplevel->get_wset() ? toplevel->get_wset()->get_index() : -1;
        description["min-size"]   = wf::ipc::dimensions_to_json(
            toplevel ? toplevel->toplevel()->get_min_size() : wf::dimensions_t{0, 0});
        description["max-size"] = wf::ipc::dimensions_to_json(
            toplevel ? toplevel->toplevel()->get_max_size() : wf::dimensions_t{0, 0});
        description["focusable"] = view->is_focusable();
        description["type"] = get_view_type(view);

        return description;
    }

    wf::ipc::method_callback list_input_devices = [&] (const nlohmann::json&)
    {
        auto response = nlohmann::json::array();
        for (auto& device : wf::get_core().get_input_devices())
        {
            nlohmann::json d;
            d["id"]     = (intptr_t)device->get_wlr_handle();
            d["name"]   = nonull(device->get_wlr_handle()->name);
            d["vendor"] = device->get_wlr_handle()->vendor;
            d["product"] = device->get_wlr_handle()->product;
            d["type"]    = wlr_input_device_type_to_string(device->get_wlr_handle()->type);
            d["enabled"] = device->is_enabled();
            response.push_back(d);
        }

        return response;
    };

    wf::ipc::method_callback configure_input_device = [&] (const nlohmann::json& data)
    {
        WFJSON_EXPECT_FIELD(data, "id", number_unsigned);
        WFJSON_EXPECT_FIELD(data, "enabled", boolean);

        for (auto& device : wf::get_core().get_input_devices())
        {
            if ((intptr_t)device->get_wlr_handle() == data["id"])
            {
                device->set_enabled(data["enabled"]);

                return wf::ipc::json_ok();
            }
        }

        return wf::ipc::json_error("Unknown input device!");
    };

    pid_t get_view_pid(wayfire_view view)
    {
        pid_t pid = -1;
        if (!view)
        {
            return pid;
        }

#if WF_HAS_XWAYLAND
        wlr_surface *wlr_surface = view->get_wlr_surface();
        if (wlr_surface && wlr_xwayland_surface_try_from_wlr_surface(wlr_surface))
        {
            pid = wlr_xwayland_surface_try_from_wlr_surface(wlr_surface)->pid;
        } else
#endif
        if (view && view->get_client())
        {
            wl_client_get_credentials(view->get_client(), &pid, 0, 0);
        }

        return pid; // NOLINT
    }
};

DECLARE_WAYFIRE_PLUGIN(ipc_rules_t);
