#ifndef PRISM2_CRYPT_H
#define PRISM2_CRYPT_H

struct hostap_crypto_ops {
	char *name;

	void * (*init)(int keyidx);

	void (*deinit)(void *priv);

	int (*encrypt_mpdu)(struct sk_buff *skb, int hdr_len, void *priv);
	int (*decrypt_mpdu)(struct sk_buff *skb, int hdr_len, void *priv);

	int (*encrypt_msdu)(struct sk_buff *skb, int hdr_len, void *priv);
	int (*decrypt_msdu)(struct sk_buff *skb, int keyidx, int hdr_len,
			    void *priv);

	int (*set_key)(void *key, int len, u8 *seq, void *priv);
	int (*get_key)(void *key, int len, u8 *seq, void *priv);

	char * (*print_stats)(char *p, void *priv);

	int extra_prefix_len, extra_postfix_len;
};

int hostap_register_crypto_ops(struct hostap_crypto_ops *ops);
int hostap_unregister_crypto_ops(struct hostap_crypto_ops *ops);
struct hostap_crypto_ops * hostap_get_crypto_ops(const char *name);

#endif 
