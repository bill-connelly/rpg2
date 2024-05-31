import rpg

config = rpg.setup()

rpg.update_shader(2.0)
shader = rpg.build_shader()

rpg.update_shader(4.0)
shader2 = rpg.build_shader()



rpg.attach_shader(shader)
rpg.display(config, shader)

rpg.attach_shader(shader2)
rpg.display(config, shader2)


#rpg.display(config)
