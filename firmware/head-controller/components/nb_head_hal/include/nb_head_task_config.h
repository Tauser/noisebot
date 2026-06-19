#ifndef NB_HEAD_TASK_CONFIG_H
#define NB_HEAD_TASK_CONFIG_H

/*
 * Tasks do head-controller. O enlace fica acima das futuras tasks de
 * render/storage, mas abaixo de qualquer gate de safety.
 */
#define NB_TASK_HEAD_LINK_PRIORITY 9U
#define NB_TASK_HEAD_LINK_STACK    4096U
#define NB_TASK_HEAD_LINK_CORE     0

#endif /* NB_HEAD_TASK_CONFIG_H */
