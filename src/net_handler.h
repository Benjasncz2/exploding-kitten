#ifndef NET_HANDLER_H
#define NET_HANDLER_H

/*
 * Protocolo de red: [uint32_t len en network-byte-order] + [JSON de 'len' bytes]
 * Retorna 0 en éxito, -1 en error.
 */
int   send_message(int fd, const char *json);

/*
 * Lee un mensaje del socket. El llamador debe liberar el puntero retornado.
 * Retorna NULL si el socket se cerró o hubo un error.
 */
char *recv_message(int fd);

#endif /* NET_HANDLER_H */
