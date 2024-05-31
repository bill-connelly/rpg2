import rpg

config = rpg.setup()
shader = rpg.build_shader()
#rpg.attach_shader(shader)
rpg.display(config, shader)
#rpg.display(config)
