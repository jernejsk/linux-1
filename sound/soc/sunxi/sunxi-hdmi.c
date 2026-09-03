// SPDX-License-Identifier: GPL-2.0-only
/*
 * Allwinner HDMI audio machine driver
 *
 * Copyright (C) 2026 Jernej Skrabec <jernej.skrabec@gmail.com>
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <sound/pcm_params.h>
#include <sound/soc.h>

#define SUNXI_HDMI_MCLK_FS	128
#define SUNXI_HDMI_SLOTS	2
#define SUNXI_HDMI_SLOT_WIDTH	32

struct sunxi_hdmi {
	struct snd_soc_card card;
	struct snd_soc_dai_link link;
	struct snd_soc_dai_link_component components[3];
	struct device_node *cpu_node;
	struct device_node *codec_node;
};

static int sunxi_hdmi_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	return snd_soc_dai_set_sysclk(cpu_dai, 0,
				      SUNXI_HDMI_MCLK_FS * params_rate(params),
				      SND_SOC_CLOCK_OUT);
}

static const struct snd_soc_ops sunxi_hdmi_ops = {
	.hw_params = sunxi_hdmi_hw_params,
};

static int sunxi_hdmi_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	return snd_soc_dai_set_tdm_slot(cpu_dai, 0, 0, SUNXI_HDMI_SLOTS,
					SUNXI_HDMI_SLOT_WIDTH);
}

static void sunxi_hdmi_put_nodes(void *data)
{
	struct sunxi_hdmi *hdmi = data;

	of_node_put(hdmi->codec_node);
	of_node_put(hdmi->cpu_node);
}

static int sunxi_hdmi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sunxi_hdmi *hdmi;
	int ret;

	hdmi = devm_kzalloc(dev, sizeof(*hdmi), GFP_KERNEL);
	if (!hdmi)
		return -ENOMEM;

	hdmi->cpu_node = of_parse_phandle(dev->of_node, "audio-cpu", 0);
	if (!hdmi->cpu_node)
		return dev_err_probe(dev, -EINVAL,
				     "missing audio-cpu phandle\n");

	hdmi->codec_node = of_parse_phandle(dev->of_node, "audio-codec", 0);
	if (!hdmi->codec_node) {
		of_node_put(hdmi->cpu_node);
		return dev_err_probe(dev, -EINVAL,
				     "missing audio-codec phandle\n");
	}

	ret = devm_add_action_or_reset(dev, sunxi_hdmi_put_nodes, hdmi);
	if (ret)
		return ret;

	hdmi->components[0].of_node = hdmi->cpu_node;
	hdmi->components[1].of_node = hdmi->codec_node;
	hdmi->components[1].dai_name = "i2s-hifi";
	hdmi->components[2].of_node = hdmi->cpu_node;

	hdmi->link.name = "HDMI";
	hdmi->link.stream_name = "HDMI PCM";
	hdmi->link.cpus = &hdmi->components[0];
	hdmi->link.num_cpus = 1;
	hdmi->link.codecs = &hdmi->components[1];
	hdmi->link.num_codecs = 1;
	hdmi->link.platforms = &hdmi->components[2];
	hdmi->link.num_platforms = 1;
	hdmi->link.playback_only = true;
	hdmi->link.dai_fmt = SND_SOC_DAIFMT_I2S |
			      SND_SOC_DAIFMT_NB_IF |
			      SND_SOC_DAIFMT_CBC_CFC;
	hdmi->link.init = sunxi_hdmi_init;
	hdmi->link.ops = &sunxi_hdmi_ops;

	hdmi->card.owner = THIS_MODULE;
	hdmi->card.dev = dev;
	hdmi->card.name = "sunxi-hdmi";
	hdmi->card.dai_link = &hdmi->link;
	hdmi->card.num_links = 1;

	ret = snd_soc_of_parse_card_name(&hdmi->card, "model");
	if (ret)
		return ret;

	return devm_snd_soc_register_card(dev, &hdmi->card);
}

static const struct of_device_id sunxi_hdmi_of_match[] = {
	{ .compatible = "allwinner,sun8i-a83t-hdmi-audio" },
	{ }
};
MODULE_DEVICE_TABLE(of, sunxi_hdmi_of_match);

static struct platform_driver sunxi_hdmi_driver = {
	.probe = sunxi_hdmi_probe,
	.driver = {
		.name = "sunxi-hdmi-audio",
		.of_match_table = sunxi_hdmi_of_match,
	},
};
module_platform_driver(sunxi_hdmi_driver);

MODULE_AUTHOR("Jernej Skrabec <jernej.skrabec@gmail.com>");
MODULE_DESCRIPTION("Allwinner HDMI audio machine driver");
MODULE_LICENSE("GPL");
