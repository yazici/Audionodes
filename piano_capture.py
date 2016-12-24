import bpy

alsa_ok = False

try:

    import alsaseq
    alsa_ok = True

except:
    pass

import time
from threading import Thread

class PianoCapture(bpy.types.Operator):
    bl_idname = "audionodes.piano"
    bl_label = "Keyboard capture"
    
    caller_id = bpy.props.StringProperty()
    caller = [None]

    def __del__(self):
        try:
            self.caller[0].clear()
        except:
            pass

    def modal(self, context, event):
        if event.type == 'ESC':
            self.caller[0].clear()
            return {'FINISHED'}
        
        #elif event.value == "RELEASE":
        #    try:
        #        self.caller[0].removeKey(("§", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "+")[("NONE", "NUMPAD_0", "NUMPAD_1", "NUMPAD_2", "NUMPAD_3", "NUMPAD_4", "NUMPAD_5", "NUMPAD_6", "NUMPAD_7", "NUMPAD_8", "NUMPAD_9", "PLUS").index(event.type)])
        #    except ValueError:
        #        pass
        #else:
        #    try:
        #        if event.unicode in ("0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "+", "§"):
        #            self.caller[0].setKey(event.unicode)
        #    except UnicodeDecodeError:
        #        pass

        return {'PASS_THROUGH'}

    def MIDIgen(self):

        if alsa_ok:

            alsaseq.client("Audionodes", 1, 1, False)
            while True:
                if alsaseq.inputpending():
                    inputData = alsaseq.input()
                    direction = inputData[0]-6 # Magic number six; key down is seven and key up is six
                    if direction in (0, 1):
                        f = 440*(2**((inputData[-1][1]-48)/12))
                        yield {"direction":("DOWN","UP")[direction], "frequency":f, "note":inputData[-1][1]}
                time.sleep(0.01)
        else:
            while True: # Never give events; sleepy loop
                time.sleep(10)

    def talkToCaller(self):
        for event in self.MIDIgen():
            self.caller[0].parseEvent(event)

    def invoke(self, context, event):
        tree = context.active_node.getTree()
        
        caller = None
        for node in tree.nodes:
            if node.path_from_id() == self.caller_id:
                caller = node
                break

        Thread(target=self.talkToCaller).start()
        self.caller[0] = caller
        
        context.window_manager.modal_handler_add(self)
        return {'RUNNING_MODAL'}


def register():
    bpy.utils.register_module(__name__)


def unregister():
    bpy.utils.unregister_module(__name__)