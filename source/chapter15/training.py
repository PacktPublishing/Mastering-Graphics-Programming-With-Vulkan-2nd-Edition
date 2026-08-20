import os
import numpy as np
import slangpy as spy
from pathlib import Path

BASE_MAT_PATH = r'D:\workspace\models\materials\Car_Paint_2k_16b\textures'
BATCH_SIZE = 256

device = spy.create_device(
    spy.DeviceType.automatic, enable_debug_layers=True, include_paths=[Path(__file__).parent]
)

module = spy.Module.load_from_file(device, "neural_materials.slang")

class WeightBiasLayer(spy.InstanceList):
    def __init__(self, inputs: int, outputs: int):
        super().__init__(module[f"WeightBiasLayer<{inputs},{outputs}>"])
        self.inputs = inputs
        self.outputs = outputs

        # Biases and weights for the layer.
        self.biases = spy.Tensor.from_numpy(device, np.zeros(outputs).astype("float32"))
        self.weights = spy.Tensor.from_numpy(
            device, np.random.uniform(-0.5, 0.5, (outputs, inputs)).astype("float32")
        )

        # Gradients for the biases and weights.
        self.biases_grad = spy.Tensor.zeros_like(self.biases)
        self.weights_grad = spy.Tensor.zeros_like(self.weights)

        # Temp data for Adam optimizer.
        self.m_biases = spy.Tensor.zeros_like(self.biases)
        self.m_weights = spy.Tensor.zeros_like(self.weights)
        self.v_biases = spy.Tensor.zeros_like(self.biases)
        self.v_weights = spy.Tensor.zeros_like(self.weights)

    # Calls the Slang 'optimize' function for biases and weights
    def optimize(self, learning_rate: float, optimize_counter: int):
        module.optimizer_step(
            self.biases,
            self.biases_grad,
            self.m_biases,
            self.v_biases,
            learning_rate,
            optimize_counter,
        )
        module.optimizer_step(
            self.weights,
            self.weights_grad,
            self.m_weights,
            self.v_weights,
            learning_rate,
            optimize_counter,
        )

class WeightLayer(spy.InstanceList):
    def __init__(self, inputs: int, outputs: int):
        super().__init__(module[f"WeightLayer<{inputs},{outputs}>"])
        self.inputs = inputs
        self.outputs = outputs

        # Biases and weights for the layer.
        self.weights = spy.Tensor.from_numpy(
            device, np.random.uniform(-0.5, 0.5, (outputs, inputs)).astype("float32")
        )

        # Gradients for the biases and weights.
        self.weights_grad = spy.Tensor.zeros_like(self.weights)

        # Temp data for Adam optimizer.
        self.m_weights = spy.Tensor.zeros_like(self.weights)
        self.v_weights = spy.Tensor.zeros_like(self.weights)

    # Calls the Slang 'optimize' function for biases and weights
    def optimize(self, learning_rate: float, optimize_counter: int):
        module.optimizer_step(
            self.weights,
            self.weights_grad,
            self.m_weights,
            self.v_weights,
            learning_rate,
            optimize_counter,
        )

class Encoder(spy.InstanceList):
    def __init__(self):
        super().__init__(module["Encoder"])
        # each sample contsist of normal, tangent, albedo, roughness and layer weight for each layer
        self.layer0 = WeightBiasLayer(11, 32)
        self.layer1 = WeightBiasLayer(32, 32)
        self.layer2 = WeightBiasLayer(32, 8)

    # Calls the Slang 'optimize' function for the layer.
    def optimize(self, learning_rate: float, optimize_counter: int):
        self.layer0.optimize(learning_rate, optimize_counter)
        self.layer1.optimize(learning_rate, optimize_counter)
        self.layer2.optimize(learning_rate, optimize_counter)

class BrdfDecoder(spy.InstanceList):
    def __init__(self):
        super().__init__(module["BrdfDecoder"])
        self.layer0 = WeightBiasLayer(20, 32)
        self.layer1 = WeightBiasLayer(32, 32)
        self.layer2 = WeightBiasLayer(32, 3) # Ignore directional albedo for now

    # Calls the Slang 'optimize' function for the layer.
    def optimize(self, learning_rate: float, optimize_counter: int):
        self.layer0.optimize(learning_rate, optimize_counter)
        self.layer1.optimize(learning_rate, optimize_counter)
        self.layer2.optimize(learning_rate, optimize_counter)

class ImportanceSamplingDecoder(spy.InstanceList):
    def __init__(self):
        super().__init__(module["ImportanceSamplingDecoder"])
        self.layer0 = WeightBiasLayer(11, 32)
        self.layer1 = WeightBiasLayer(32, 32)
        self.layer2 = WeightBiasLayer(32, 32)
        self.layer3 = WeightBiasLayer(32, 9)

    # Calls the Slang 'optimize' function for the layer.
    def optimize(self, learning_rate: float, optimize_counter: int):
        self.layer0.optimize(learning_rate, optimize_counter)
        self.layer1.optimize(learning_rate, optimize_counter)
        self.layer2.optimize(learning_rate, optimize_counter)
        self.layer3.optimize(learning_rate, optimize_counter)

class FrameDecoder(spy.InstanceList):
    def __init__(self):
        super().__init__(module["FrameDecoder"])
        self.layer0 = WeightLayer(8, 12)

    # Calls the Slang 'optimize' function for the layer.
    def optimize(self, learning_rate: float, optimize_counter: int):
        self.layer0.optimize(learning_rate, optimize_counter)

class LatentTexture(spy.InstanceList):
    def __init__(self, width: int, height: int):
        super().__init__(module[f"LatentTexture"])
        self.width = width
        self.height = height

        # Initialize to random latent texture
        # initial_latents = np.random.uniform(0.0, 1.0, (height, width)).astype("float32")
        self.texture = spy.Tensor.zeros(device, (height, width), dtype="vector<float,4>")

        # Gradients for the latent texture
        self.texture_grads = spy.Tensor.zeros_like(self.texture)

        # Temp data for Adam optimizer.
        self.m_texture = spy.Tensor.zeros_like(self.texture)
        self.v_texture = spy.Tensor.zeros_like(self.texture)

    # Calls the Slang 'optimize' function for biases and weights
    def optimize(self, learning_rate: float, optimize_counter: int):
        if self.width == 2048:
            module.texture_optimizer_step.call_group_shape(spy.slangpy.Shape(16, 16))(
                self.texture,
                self.texture_grads,
                self.m_texture,
                self.v_texture,
                learning_rate,
                optimize_counter,
            )
        else:
            module.texture_optimizer_step(
                self.texture,
                self.texture_grads,
                self.m_texture,
                self.v_texture,
                learning_rate,
                optimize_counter,
            )

class LatentTexturePyramid(spy.InstanceList):
    def __init__(self, width: int, height: int, levels: int):
        super().__init__(module[f"LatentTexturePyramid<{levels}>"])
        self.levels = levels

        self.textures0 = []
        self.textures1 = []
        for i in range(self.levels):
            level_width = max(1, width // (2 ** i))
            level_height = max(1, height // (2 ** i))
            self.textures0.append(LatentTexture(level_width, level_height))
            self.textures1.append(LatentTexture(level_width, level_height))

    def optimize(self, learning_rate: float, optimize_counter: int):
        for i in range(self.levels):
            self.textures0[i].optimize(learning_rate, optimize_counter)
            self.textures1[i].optimize(learning_rate, optimize_counter)

if __name__ == "__main__":

    # Load some materials.
    metallic_map = spy.Tensor.load_from_image(
        device, os.path.join(BASE_MAT_PATH, 'Car_Paint_metallic.png'), grayscale=True
    )
    normal_map = spy.Tensor.load_from_image(
        device, os.path.join(BASE_MAT_PATH, 'Car_Paint_normal.png'), scale=2, offset=-1
    )
    roughness_map = spy.Tensor.load_from_image(
        device, os.path.join(BASE_MAT_PATH, 'Car_Paint_roughness.png'), grayscale=True
    )

    encoder = Encoder()
    frame_decoder = FrameDecoder()
    brdf_decoder = BrdfDecoder()
    is_decoder = ImportanceSamplingDecoder()

    mip_levels = 1
    w = normal_map.shape[1] // 2
    h = normal_map.shape[0] // 2
    while w >= 1 and h >= 1:
        mip_levels += 1
        w //= 2
        h //= 2

    latent_texture_pyramid = LatentTexturePyramid(width=normal_map.shape[1], height=normal_map.shape[0], levels=mip_levels)

    batch_kx = spy.Tensor.zeros(device, (BATCH_SIZE * 11,), dtype=float)
    brdf_loss = spy.Tensor.zeros(device, (BATCH_SIZE,), dtype=float)

    steps = 300000
    # After these many steps, we switch from training the encoder to training the latent texture.
    latent_switch_step = 20000
    # TODO: learning rate schedule
    learning_rate = 1e-3
    # TODO: mollification
    for epoch in range(steps):
        # Simple example first: just one layer. The random UVs and view directions are computed on the GPU
        # Each thread/lane is a batch entry

        if epoch == latent_switch_step:
            # Bake encoder outputs into level 0 of the latent texture pyramid.
            module.copy_encoder_to_latent_texture.call_group_shape(spy.slangpy.Shape(16, 16))(
                encoder=encoder,
                latent_texture_pyramid=latent_texture_pyramid,
                batch_index=spy.grid((normal_map.shape[1], normal_map.shape[0])),
                normal_map=normal_map,
                roughness_map=roughness_map,
            )

            # Populate coarser MIP levels by box-filtering from the level above.
            w, h = normal_map.shape[1], normal_map.shape[0]
            for level in range(mip_levels - 1):
                w //= 2
                h //= 2
                module.downsample_latent_pyramid(
                    latent_texture_pyramid=latent_texture_pyramid,
                    batch_index=spy.grid((w, h)),
                    src_level=level,
                    dst_level=level + 1,
                )

        seed = spy.wang_hash(seed=epoch, warmup=2)

        module.compute_random_batch(
            epoch=epoch,
            seed=seed,
            batch_index=spy.grid((BATCH_SIZE,)),
            batch_size=BATCH_SIZE,
            output=batch_kx,
            normal_map=normal_map,
            roughness_map=roughness_map,
        )

        if epoch < latent_switch_step:
            # Train the encoder
            module.train_brdf_with_encoder(
                seed=seed,
                k=batch_kx,
                batch_index=spy.grid((BATCH_SIZE,)),
                batch_size=BATCH_SIZE,
                encoder=encoder,
                frame_decoder=frame_decoder,
                brdf_decoder=brdf_decoder,
                loss_output=brdf_loss
            )

            encoder.optimize(learning_rate=learning_rate, optimize_counter=epoch + 1)
        else:
            module.train_brdf_with_latent_texture(
                seed=seed,
                k=batch_kx,
                batch_index=spy.grid((BATCH_SIZE,)),
                batch_size=BATCH_SIZE,
                latent_texture_pyramid=latent_texture_pyramid,
                frame_decoder=frame_decoder,
                brdf_decoder=brdf_decoder,
                loss_output=brdf_loss
            )

            latent_texture_pyramid.optimize(learning_rate=learning_rate, optimize_counter=epoch + 1)

        frame_decoder.optimize(learning_rate=learning_rate, optimize_counter=epoch + 1)
        brdf_decoder.optimize(learning_rate=learning_rate, optimize_counter=epoch + 1)

        if epoch % 100 == 0:
            print(f"Epoch: {epoch}  Loss: {np.mean(brdf_loss.to_numpy()):.5f}")

        # module.compute_random_batch(
        #     epoch=epoch,
        #     seed=spy.wang_hash(seed=epoch + steps, warmup=2),
        #     batch_index=spy.grid((BATCH_SIZE,)),
        #     batch_size=BATCH_SIZE,
        #     output=batch_kx,
        #     normal_map=normal_map,
        #     roughness_map=roughness_map,
        # )

        # if epoch < latent_switch_step:
        #     module.train_sampler_with_encoder(
        #         seed=seed,
        #         input_values=batch_kx,
        #         batch_index=spy.grid((BATCH_SIZE,)),
        #         batch_size=BATCH_SIZE,
        #         encoder=encoder,
        #         brdf_decoder=brdf_decoder,
        #         sampler_decoder=is_decoder,
        #     )

        #     is_decoder.optimize(learning_rate=learning_rate, optimize_counter=epoch)
        # else:
        #     module.train_sampler_with_latent_texture(
        #         seed=seed,
        #         input_values=batch_kx,
        #         batch_index=spy.grid((BATCH_SIZE,)),
        #         batch_size=BATCH_SIZE,
        #         latent_texture_pyramid=latent_texture_pyramid,
        #         brdf_decoder=brdf_decoder,
        #         sampler_decoder=is_decoder,
        #     )

        #     is_decoder.optimize(learning_rate=learning_rate, optimize_counter=epoch)
