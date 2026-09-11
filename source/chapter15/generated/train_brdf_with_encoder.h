namespace train_brdf_with_encoder {
namespace set_1 {

static idra::DescriptorSetLayoutHandle dsl;

static idra::DescriptorSetLayoutHandle create_descriptor_set_layout( idra::GpuDevice* gpu ) {

	dsl = gpu->create_descriptor_set_layout( {
		.bindings = {
		},
		.dynamic_buffer_bindings = { 0, 1, },
		.debug_name="train_brdf_with_encoder_dsl" });
	return dsl;
}

static idra::DynamicBufferBinding dynamic_buffers_dsc[] = {  { 0, 0 },  { 1, 0 }, };

static void dynamic_buffers(  u32 g_random_batch_constants_size, u32 g_brdf_training_constants_size) {
	dynamic_buffers_dsc[ 0 ].size = g_random_batch_constants_size;
	dynamic_buffers_dsc[ 1 ].size = g_brdf_training_constants_size;
}


static idra::DescriptorSetHandle create_descriptor_set( idra::GpuDevice* gpu ) {
	return gpu->create_descriptor_set( {
		.dynamic_buffer_bindings = dynamic_buffers_dsc,
		.layout = dsl,
		.debug_name = "train_brdf_with_encoder_dsl"} );
}
} // namespace set_1 
} // train_brdf_with_encoder
