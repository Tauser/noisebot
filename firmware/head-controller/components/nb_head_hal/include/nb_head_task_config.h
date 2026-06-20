#ifndef NB_HEAD_TASK_CONFIG_H
#define NB_HEAD_TASK_CONFIG_H

/*
 * Tasks do head-controller. O enlace fica acima das futuras tasks de
 * render/storage, mas abaixo de qualquer gate de safety.
 */
#define NB_TASK_HEAD_LINK_PRIORITY 9U
#define NB_TASK_HEAD_LINK_STACK    4096U
#define NB_TASK_HEAD_LINK_CORE     0

#define NB_TASK_HEAD_DISPLAY_PRIORITY 7U
#define NB_TASK_HEAD_DISPLAY_STACK    4096U
#define NB_TASK_HEAD_DISPLAY_CORE     1

/*
 * Captura V4L2 pode bloquear por centenas de ms (VIDIOC_DQBUF, retries).
 * Prioridade abaixo do enlace e do display para nunca atrasar ACK/heartbeat;
 * a task do enlace só enfileira o comando e segue (ver DM4_BRINGUP.md).
 */
#define NB_TASK_HEAD_CAMERA_PRIORITY  6U
#define NB_TASK_HEAD_CAMERA_STACK     4096U
#define NB_TASK_HEAD_CAMERA_CORE      1

#endif /* NB_HEAD_TASK_CONFIG_H */
