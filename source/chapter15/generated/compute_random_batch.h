namespace compute_random_batch {
namespace set_1 {

static raptor::DescriptorSetLayoutHandle dsl;

static raptor::DescriptorSetLayoutHandle create_descriptor_set_layout( raptor::GpuDevice* gpu ) {

	dsl = gpu->create_descriptor_set_layout( {
		.bindings = {
			{ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
		},
		.set_index = 1,
		.name="compute_random_batch_dsl" });
	return dsl;
}

static raptor::DynamicBufferBinding dynamic_buffers_dsc[] = {  { 0, 0 }, };

static void dynamic_buffers(  u32 g_random_batch_constants_size) {
	dynamic_buffers_dsc[ 0 ].size = g_random_batch_constants_size;
}


static raptor::DescriptorSetHandle create_descriptor_set( raptor::GpuDevice* gpu ) {
	return gpu->create_descriptor_set( {
		.dynamic_buffers = dynamic_buffers_dsc,
		.layout = dsl,
		.name = "compute_random_batch_dsl"} );
}
} // namespace set_1 
} // compute_random_batch
