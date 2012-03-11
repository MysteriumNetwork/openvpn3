#ifndef OPENVPN_POLARSSL_UTIL_RAND_H
#define OPENVPN_POLARSSL_UTIL_RAND_H

#include <polarssl/entropy_poll.h>
#include <polarssl/ctr_drbg.h>

#include <openvpn/common/types.hpp>
#include <openvpn/random/randbase.hpp>

namespace openvpn {

  class RandomPolarSSL : public RandomBase {
  public:
    RandomPolarSSL()
    {
      if (ctr_drbg_init(&ctx, entropy_poll, NULL, NULL, 0) < 0)
	throw rand_error("CTR_DRBG init");
    }

    virtual const char *name() const {
      return "CTR_DRBG";
    }

    virtual void rand_bytes(unsigned char *buf, const size_t size)
    {
      if (ctr_drbg_random(&ctx, buf, size) < 0)
	throw rand_error("CTR_DRBG rand_bytes");
    }

  private:
    static int entropy_poll(void *data, unsigned char *output, size_t len)
    {
      size_t olen;
      return platform_entropy_poll(data, output, len, &olen);
    }

    ctr_drbg_context ctx;
  };

}

#endif
