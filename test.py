import rpg
import time

config = rpg.setup(16)

#shader = rpg.build_shader(0, 20.1, 1)
#rpg.load_shader(config, shader)
#rpg.display(config, 25)

#time.sleep(2)

# if (!PyArg_ParseTuple(args, "fff", &angle, &cyclesPerPixel, &cyclesPerSecond)) {

shader2 = rpg.build_shader(3.141/4, 20.1, 5 )
rpg.load_shader(config, shader2)
rpg.display(config, 0)
#rpg.display(config, 25)



#rpg.update_shader(shader2, 3.141/4.0)


#rpg.attach_shader(shader2)
#rpg.display(config, shader2)


#rpg.display(config)
