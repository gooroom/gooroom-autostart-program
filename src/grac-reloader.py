#! /usr/bin/env python3

#-----------------------------------------------------------------------
import dbus
import time

#-----------------------------------------------------------------------
SD_BUS_NAME = 'org.freedesktop.systemd1'
SD_BUS_OBJ = '/org/freedesktop/systemd1'
SD_BUS_IFACE = 'org.freedesktop.systemd1.Manager'
SD_BUS_PROP='org.freedesktop.DBus.Properties'
SD_BUS_UNIT='org.freedesktop.systemd1.Unit'

#-----------------------------------------------------------------------
def reload_service(service):
    """
    systemctl reload service
    """

    bus = dbus.SystemBus()
    systemd1 = bus.get_object(SD_BUS_NAME, SD_BUS_OBJ)
    manager = dbus.Interface(systemd1, SD_BUS_IFACE)

    manager.ReloadUnit(service, "fail")

    return wait_status_updated(bus, manager, service, 'active', 5)

#-----------------------------------------------------------------------
def wait_status_updated(bus, manager, service, status, timeout):
    """
    wait service status updated
    """

    for i in range(timeout):
        if service_state(bus, manager, service) == status:
            return 'OK'

        time.sleep(1)

    raise Exception('SYSTEMD TIMEOUT')

#-----------------------------------------------------------------------
def service_state(bus, manager, service):
    """
    systemctl status service
    """

    unit = manager.GetUnit(service)
    unit_obj = bus.get_object(SD_BUS_NAME, unit)
    unit_prop = dbus.Interface(unit_obj, SD_BUS_PROP)

    active = unit_prop.Get(SD_BUS_UNIT, 'ActiveState')

    return active

#-----------------------------------------------------------------------
if __name__ == '__main__':

    service = 'grac-device-daemon.service'

    try:
        reload_service(service)
        print('SUCCESS')
    except:
        print('FAILURE')
        
