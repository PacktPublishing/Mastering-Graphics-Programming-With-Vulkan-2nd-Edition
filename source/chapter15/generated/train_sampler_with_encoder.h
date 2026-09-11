namespace train_sampler_with_encoder {
namespace set_1 {

static idra::DescriptorSetLayoutHandle dsl;

static idra::DescriptorSetLayoutHandle create_descriptor_set_layout( idra::GpuDevice* gpu ) {

	dsl = gpu->create_descriptor_set_layout( {
		.bindings = {
		},
		.dynamic_buffer_bindings = { 0, },
		.debug_name="train_sampler_with_encoder_dsl" });
	return dsl;
}

static idra::DynamicBufferBinding dynamic_buffers_dsc[] = {  { 0, 0 }, };

static void dynamic_buffers(  u32 g_sampler_training_constants_size) {
	dynamic_buffers_dsc[ 0 ].size = g_sampler_training_constants_size;
}


static idra::DescriptorSetHandle create_descriptor_set( idra::GpuDevice* gpu ) {
	return gpu->create_descriptor_set( {
		.dynamic_buffer_bindings = dynamic_buffers_dsc,
		.layout = dsl,
		.debug_name = "train_sampler_with_encoder_ds"} );
}
} // namespace set_1
} // train_sampler_with_encoder
