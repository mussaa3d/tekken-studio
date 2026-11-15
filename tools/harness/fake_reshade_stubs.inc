// Made by make_reshade_stubs.py from the ReShade headers in third_party/reshade. Don't edit: run the script again.

struct StubRuntime : effect_runtime {
    // api_object
    uint64_t get_native() const override { return {}; }
    void get_private_data(const uint8_t guid[16], uint64_t *data) const override {}
    void set_private_data(const uint8_t guid[16], const uint64_t data) override {}
    // device_object
    device *get_device() override { return {}; }
    // effect_runtime
    void *get_hwnd() const override { return {}; }
    resource get_back_buffer(uint32_t index) override { return {}; }
    uint32_t get_back_buffer_count() const override { return {}; }
    uint32_t get_current_back_buffer_index() const override { return {}; }
    command_queue *get_command_queue() override { return {}; }
    void render_effects(command_list *cmd_list, resource_view rtv, resource_view rtv_srgb) override {}
    bool capture_screenshot(void *pixels) override { return {}; }
    void get_screenshot_width_and_height(uint32_t *out_width, uint32_t *out_height) const override {}
    bool is_key_down(uint32_t keycode) const override { return {}; }
    bool is_key_pressed(uint32_t keycode) const override { return {}; }
    bool is_key_released(uint32_t keycode) const override { return {}; }
    bool is_mouse_button_down(uint32_t button) const override { return {}; }
    bool is_mouse_button_pressed(uint32_t button) const override { return {}; }
    bool is_mouse_button_released(uint32_t button) const override { return {}; }
    void get_mouse_cursor_position(uint32_t *out_x, uint32_t *out_y, int16_t *out_wheel_delta = nullptr) const override {}
    void enumerate_uniform_variables(const char *effect_name, void(*callback)(effect_runtime *runtime, effect_uniform_variable variable, void *user_data), void *user_data) override {}
    effect_uniform_variable find_uniform_variable(const char *effect_name, const char *variable_name) const override { return {}; }
    void get_uniform_variable_type(effect_uniform_variable variable, format *out_base_type, uint32_t *out_rows = nullptr, uint32_t *out_columns = nullptr, uint32_t *out_array_length = nullptr) const override {}
    void get_uniform_variable_name(effect_uniform_variable variable, char *name, size_t *name_size) const override {}
    bool get_annotation_bool_from_uniform_variable(effect_uniform_variable variable, const char *name, bool *values, size_t count, size_t array_index = 0) const override { return {}; }
    bool get_annotation_float_from_uniform_variable(effect_uniform_variable variable, const char *name, float *values, size_t count, size_t array_index = 0) const override { return {}; }
    bool get_annotation_int_from_uniform_variable(effect_uniform_variable variable, const char *name, int32_t *values, size_t count, size_t array_index = 0) const override { return {}; }
    bool get_annotation_uint_from_uniform_variable(effect_uniform_variable variable, const char *name, uint32_t *values, size_t count, size_t array_index = 0) const override { return {}; }
    bool get_annotation_string_from_uniform_variable(effect_uniform_variable variable, const char *name, char *value, size_t *value_size) const override { return {}; }
    void get_uniform_value_bool(effect_uniform_variable variable, bool *values, size_t count, size_t array_index = 0) const override {}
    void get_uniform_value_float(effect_uniform_variable variable, float *values, size_t count, size_t array_index = 0) const override {}
    void get_uniform_value_int(effect_uniform_variable variable, int32_t *values, size_t count, size_t array_index = 0) const override {}
    void get_uniform_value_uint(effect_uniform_variable variable, uint32_t *values, size_t count, size_t array_index = 0) const override {}
    void set_uniform_value_bool(effect_uniform_variable variable, const bool *values, size_t count, size_t array_index = 0) override {}
    void set_uniform_value_float(effect_uniform_variable variable, const float *values, size_t count, size_t array_index = 0) override {}
    void set_uniform_value_int(effect_uniform_variable variable, const int32_t *values, size_t count, size_t array_index = 0) override {}
    void set_uniform_value_uint(effect_uniform_variable variable, const uint32_t *values, size_t count, size_t array_index = 0) override {}
    void enumerate_texture_variables(const char *effect_name, void(*callback)(effect_runtime *runtime, effect_texture_variable variable, void *user_data), void *user_data) override {}
    effect_texture_variable find_texture_variable(const char *effect_name, const char *variable_name) const override { return {}; }
    void get_texture_variable_name(effect_texture_variable variable, char *name, size_t *name_size) const override {}
    bool get_annotation_bool_from_texture_variable(effect_texture_variable variable, const char *name, bool *values, size_t count, size_t array_index = 0) const override { return {}; }
    bool get_annotation_float_from_texture_variable(effect_texture_variable variable, const char *name, float *values, size_t count, size_t array_index = 0) const override { return {}; }
    bool get_annotation_int_from_texture_variable(effect_texture_variable variable, const char *name, int32_t *values, size_t count, size_t array_index = 0) const override { return {}; }
    bool get_annotation_uint_from_texture_variable(effect_texture_variable variable, const char *name, uint32_t *values, size_t count, size_t array_index = 0) const override { return {}; }
    bool get_annotation_string_from_texture_variable(effect_texture_variable variable, const char *name, char *value, size_t *value_size) const override { return {}; }
    void update_texture(effect_texture_variable variable, const uint32_t width, const uint32_t height, const void *pixels) override {}
    void get_texture_binding(effect_texture_variable variable, resource_view *out_srv, resource_view *out_srv_srgb) const override {}
    void update_texture_bindings(const char *semantic, resource_view srv, resource_view srv_srgb) override {}
    void enumerate_techniques(const char *effect_name, void(*callback)(effect_runtime *runtime, effect_technique technique, void *user_data), void *user_data) override {}
    effect_technique find_technique(const char *effect_name, const char *technique_name) override { return {}; }
    void get_technique_name(effect_technique technique, char *name, size_t *name_size) const override {}
    bool get_annotation_bool_from_technique(effect_technique technique, const char *name, bool *values, size_t count, size_t array_index = 0) const override { return {}; }
    bool get_annotation_float_from_technique(effect_technique technique, const char *name, float *values, size_t count, size_t array_index = 0) const override { return {}; }
    bool get_annotation_int_from_technique(effect_technique technique, const char *name, int32_t *values, size_t count, size_t array_index = 0) const override { return {}; }
    bool get_annotation_uint_from_technique(effect_technique technique, const char *name, uint32_t *values, size_t count, size_t array_index = 0) const override { return {}; }
    bool get_annotation_string_from_technique(effect_technique technique, const char *name, char *value, size_t *value_size) const override { return {}; }
    bool get_technique_state(effect_technique technique) const override { return {}; }
    void set_technique_state(effect_technique technique, bool enabled) override {}
    bool get_preprocessor_definition(const char *name, char *value, size_t *value_size) const override { return {}; }
    void set_preprocessor_definition(const char *name, const char *value) override {}
    void render_technique(effect_technique technique, command_list *cmd_list, resource_view rtv, resource_view rtv_srgb = { 0 }) override {}
    bool get_effects_state() const override { return {}; }
    void set_effects_state(bool enabled) override {}
    void get_current_preset_path(char *path, size_t *path_size) const override {}
    void set_current_preset_path(const char *path) override {}
    void reorder_techniques(size_t count, const effect_technique *techniques) override {}
    void block_input_next_frame() override {}
    uint32_t last_key_pressed() const override { return {}; }
    uint32_t last_key_released() const override { return {}; }
    void get_uniform_variable_effect_name(effect_uniform_variable variable, char *effect_name, size_t *effect_name_size) const override {}
    void get_texture_variable_effect_name(effect_texture_variable variable, char *effect_name, size_t *effect_name_size) const override {}
    void get_technique_effect_name(effect_technique technique, char *effect_name, size_t *effect_name_size) const override {}
    void save_current_preset() const override {}
    bool get_preprocessor_definition_for_effect(const char *effect_name, const char *name, char *value, size_t *value_size) const override { return {}; }
    void set_preprocessor_definition_for_effect(const char *effect_name, const char *name, const char *value) override {}
    bool open_overlay(bool open, input_source source) override { return {}; }
    void set_color_space(color_space color_space) override {}
    void reset_uniform_value(effect_uniform_variable variable) override {}
    void reload_effect_next_frame(const char *effect_name) override {}
    void export_current_preset(const char *path) const override {}
    void save_screenshot(const char *postfix = nullptr) override {}
};

struct StubDevice : device {
    // api_object
    uint64_t get_native() const override { return {}; }
    void get_private_data(const uint8_t guid[16], uint64_t *data) const override {}
    void set_private_data(const uint8_t guid[16], const uint64_t data) override {}
    // device
    device_api get_api() const override { return {}; }
    bool check_capability(device_caps capability) const override { return {}; }
    bool check_format_support(format format, resource_usage usage) const override { return {}; }
    bool create_sampler(const sampler_desc &desc, sampler *out_sampler) override { return {}; }
    void destroy_sampler(sampler sampler) override {}
    bool create_resource(const resource_desc &desc, const subresource_data *initial_data, resource_usage initial_state, resource *out_resource, void **shared_handle = nullptr) override { return {}; }
    void destroy_resource(resource resource) override {}
    resource_desc get_resource_desc(resource resource) const override { return {}; }
    bool create_resource_view(resource resource, resource_usage usage_type, const resource_view_desc &desc, resource_view *out_view) override { return {}; }
    void destroy_resource_view(resource_view view) override {}
    resource get_resource_from_view(resource_view view) const override { return {}; }
    resource_view_desc get_resource_view_desc(resource_view view) const override { return {}; }
    bool map_buffer_region(resource resource, uint64_t offset, uint64_t size, map_access access, void **out_data) override { return {}; }
    void unmap_buffer_region(resource resource) override {}
    bool map_texture_region(resource resource, uint32_t subresource, const subresource_box *box, map_access access, subresource_data *out_data) override { return {}; }
    void unmap_texture_region(resource resource, uint32_t subresource) override {}
    void update_buffer_region(const void *data, resource dest, uint64_t dest_offset, uint64_t size) override {}
    void update_texture_region(const subresource_data &data, resource dest, uint32_t dest_subresource, const subresource_box *dest_box = nullptr) override {}
    bool create_pipeline(pipeline_layout layout, uint32_t subobject_count, const pipeline_subobject *subobjects, pipeline *out_pipeline) override { return {}; }
    void destroy_pipeline(pipeline pipeline) override {}
    bool create_pipeline_layout(uint32_t param_count, const pipeline_layout_param *params, pipeline_layout *out_layout) override { return {}; }
    void destroy_pipeline_layout(pipeline_layout layout) override {}
    bool allocate_descriptor_tables(uint32_t count, pipeline_layout layout, uint32_t param, descriptor_table *out_tables) override { return {}; }
    void free_descriptor_tables(uint32_t count, const descriptor_table *tables) override {}
    void get_descriptor_heap_offset(descriptor_table table, uint32_t binding, uint32_t array_offset, descriptor_heap *out_heap, uint32_t *out_offset) const override {}
    void copy_descriptor_tables(uint32_t count, const descriptor_table_copy *copies) override {}
    void update_descriptor_tables(uint32_t count, const descriptor_table_update *updates) override {}
    bool create_query_heap(query_type type, uint32_t count, query_heap *out_heap) override { return {}; }
    void destroy_query_heap(query_heap heap) override {}
    bool get_query_heap_results(query_heap heap, query_type type, uint32_t first, uint32_t count, void *results, uint32_t stride) override { return {}; }
    void set_resource_name(resource resource, const char *name) override {}
    void set_resource_view_name(resource_view view, const char *name) override {}
    bool create_fence(uint64_t initial_value, fence_flags flags, fence *out_fence, void **shared_handle = nullptr) override { return {}; }
    void destroy_fence(fence fence) override {}
    uint64_t get_completed_fence_value(fence fence) const override { return {}; }
    bool wait(fence fence, uint64_t value, uint64_t timeout = UINT64_MAX) override { return {}; }
    bool signal(fence fence, uint64_t value) override { return {}; }
    bool get_property(device_properties property, void *data) const override { return {}; }
    uint64_t get_resource_view_gpu_address(resource_view view) const override { return {}; }
    void get_acceleration_structure_size(acceleration_structure_type type, acceleration_structure_build_flags flags, uint32_t input_count, const acceleration_structure_build_input *inputs, uint64_t *out_size, uint64_t *out_build_scratch_size, uint64_t *out_update_scratch_size) const override {}
    bool get_pipeline_shader_group_handles(pipeline pipeline, uint32_t first, uint32_t count, void *out_handles) override { return {}; }
};

struct StubQueue : command_queue {
    // api_object
    uint64_t get_native() const override { return {}; }
    void get_private_data(const uint8_t guid[16], uint64_t *data) const override {}
    void set_private_data(const uint8_t guid[16], const uint64_t data) override {}
    // device_object
    device *get_device() override { return {}; }
    // command_queue
    command_queue_type get_type() const override { return {}; }
    void wait_idle() const override {}
    void flush_immediate_command_list() const override {}
    command_list *get_immediate_command_list() override { return {}; }
    void begin_debug_event(const char *label, const float color[4] = nullptr) override {}
    void end_debug_event() override {}
    void insert_debug_marker(const char *label, const float color[4] = nullptr) override {}
    bool wait(fence fence, uint64_t value) override { return {}; }
    bool signal(fence fence, uint64_t value) override { return {}; }
    uint64_t get_timestamp_frequency() const override { return {}; }
};

