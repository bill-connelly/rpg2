import rpg
import time

config = rpg.setup()

#shader = rpg.build_shader(0, 20.1, 1)
#rpg.load_shader(config, shader)
#rpg.display(config, 25)

#time.sleep(2)

shader2 = rpg.build_shader(0, 5.01, 2)
rpg.load_shader(config, shader2)
rpg.thread_display(config)
#rpg.display(config, 25)



#rpg.update_shader(shader2, 3.141/4.0)


#rpg.attach_shader(shader2)
#rpg.display(config, shader2)


#rpg.display(config)
