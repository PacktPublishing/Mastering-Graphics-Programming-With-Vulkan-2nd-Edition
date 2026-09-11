namespace train_brdf_with_encoder {
namespace set_1 {

static raptor::DescriptorSetLayoutHandle dsl;

static raptor::DescriptorSetLayoutHandle create_descriptor_set_layout( raptor::GpuDevice* gpu ) {

	dsl = gpu->create_descriptor_set_layout( {
		.bindings = {
			{ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
			{ 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
		},
		.set_index = 1,
		.name="compute_train_brdf_with_encoder_dsl" });
	return dsl;
}

static raptor::DynamicBufferBinding dynamic_buffers_dsc[] = {  { 0, 0 }, { 1, 0 } };

static void dynamic_buffers(  u32 g_random_batch_constants_size, u32 g_brdf_training_constants_size) {
	dynamic_buffers_dsc[ 0 ].size = g_random_batch_constants_size;
	dynamic_buffers_dsc[ 1 ].size = g_brdf_training_constants_size;
}


static raptor::DescriptorSetHandle create_descriptor_set( raptor::GpuDevice* gpu ) {
	return gpu->create_descriptor_set( {
		.dynamic_buffers = dynamic_buffers_dsc,
		.layout = dsl,
		.name = "compute_train_brdf_with_encoder_ds"} );
}
} // namespace set_1
} // train_brdf_with_encoder
