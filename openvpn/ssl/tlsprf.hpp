#ifndef OPENVPN_SSL_TLSPRF_H
#define OPENVPN_SSL_TLSPRF_H

#include <cstring>

#ifdef OPENVPN_DEBUG
#include <string>
#include <sstream>
#include <openvpn/common/hexstr.hpp>
#endif

#include <openvpn/common/exception.hpp>
#include <openvpn/buffer/buffer.hpp>
#include <openvpn/random/randbase.hpp>
#include <openvpn/crypto/static_key.hpp>
#include <openvpn/ssl/psid.hpp>
#include <openvpn/gencrypto/evphmac.hpp>

namespace openvpn {

  class TLSPRF
  {
  public:
    OPENVPN_SIMPLE_EXCEPTION(tlsprf_uninitialized);
    OPENVPN_SIMPLE_EXCEPTION(tlsprf_client_server_mismatch);

    TLSPRF(const bool server)
      : initialized_(false), server_(server) {}

    void randomize(RandomBase& rng)
    {
      if (!server_)
	rng.rand_bytes(pre_master, sizeof(pre_master));
      rng.rand_bytes(random1, sizeof(random1));
      rng.rand_bytes(random2, sizeof(random2));
      initialized_ = true;
    }

    void read(Buffer& buf)
    {
      if (!server_)
	buf.read(pre_master, sizeof(pre_master));
      buf.read(random1, sizeof(random1));
      buf.read(random2, sizeof(random2));
      initialized_ = true;
    }

    void write(Buffer& buf)
    {
      verify_initialized();
      if (!server_)
	buf.write(pre_master, sizeof(pre_master));
      buf.write(random1, sizeof(random1));
      buf.write(random2, sizeof(random2));
    }

    void generate_key_expansion(OpenVPNStaticKey& dest, const TLSPRF& peer,
				const ProtoSessionID& psid_self, const ProtoSessionID& psid_peer) const
    {
      if (server_ == peer.server_)
	throw tlsprf_client_server_mismatch();
      if (server_)
	gen_exp(dest, peer, psid_peer, *this, psid_self);
      else
	gen_exp(dest, *this, psid_self, peer, psid_peer);
    }

    void erase()
    {
      if (initialized_)
	{
	  if (!server_)
	    std::memset(pre_master, 0, sizeof(pre_master));
	  std::memset(random1, 0, sizeof(random1));
	  std::memset(random2, 0, sizeof(random2));
	  initialized_ = false;
	}
    }

#ifdef OPENVPN_DEBUG
    std::string dump(const char *title)
    {
      std::ostringstream out;
      out << "*** TLSPRF " << title << " pre_master: " << render_hex(pre_master, sizeof(pre_master)) << std::endl;
      out << "*** TLSPRF " << title << " random1: " << render_hex(random1, sizeof(random1)) << std::endl;
      out << "*** TLSPRF " << title << " random2: " << render_hex(random2, sizeof(random2)) << std::endl;
      return out.str();
    }
#endif

    ~TLSPRF()
    {
      erase();
    }

    static void openvpn_PRF (const unsigned char *secret,
			     const size_t secret_len,
			     const char *label,
			     const unsigned char *client_seed,
			     const size_t client_seed_len,
			     const unsigned char *server_seed,
			     const size_t server_seed_len,
			     const ProtoSessionID* client_sid,
			     const ProtoSessionID* server_sid,
			     unsigned char *output,
			     const size_t output_len)
    {
      const size_t label_len = strlen(label);
      BufferAllocated seed(label_len
			   + client_seed_len
			   + server_seed_len
			   + ProtoSessionID::SIZE * 2,
			   BufferAllocated::DESTRUCT_ZERO);
      seed.write((unsigned char *)label, label_len);
      seed.write(client_seed, client_seed_len);
      seed.write(server_seed, server_seed_len);
      if (client_sid)
	client_sid->write(seed);
      if (server_sid)
	server_sid->write(seed);

      // compute PRF
      PRF(seed.data(), seed.size(), secret, secret_len, output, output_len);
    }

  private:
    /*
     * Use the TLS PRF function for generating data channel keys.
     * This code is adapted from the OpenSSL library.
     *
     * TLS generates keys as such:
     *
     * master_secret[48] = PRF(pre_master_secret[48], "master secret",
     *                         ClientHello.random[32] + ServerHello.random[32])
     *
     * key_block[] = PRF(SecurityParameters.master_secret[48],
     *                 "key expansion",
     *                 SecurityParameters.server_random[32] +
     *                 SecurityParameters.client_random[32]);
     *
     * Notes:
     *
     * (1) key_block contains a full set of 4 keys.
     * (2) The pre-master secret is generated by the client.
     */

    static void hash (const EVP_MD *md,
		      const unsigned char *sec,
		      const size_t sec_len,
		      const unsigned char *seed,
		      const size_t seed_len,
		      unsigned char *out,
		      size_t olen)
    {
      size_t chunk;
      unsigned int j;
      HMAC_CTX ctx;
      HMAC_CTX ctx_tmp;
      unsigned char A1[EVP_MAX_MD_SIZE];
      unsigned int A1_len;

      chunk=EVP_MD_size(md);

      HMAC_CTX_init(&ctx);
      HMAC_CTX_init(&ctx_tmp);
      HMAC_Init_ex(&ctx,sec,sec_len,md, NULL);
      HMAC_Init_ex(&ctx_tmp,sec,sec_len,md, NULL);
      HMAC_Update(&ctx,seed,seed_len);
      HMAC_Final(&ctx,A1,&A1_len);

      for (;;)
	{
	  HMAC_Init_ex(&ctx,NULL,0,NULL,NULL); /* re-init */
	  HMAC_Init_ex(&ctx_tmp,NULL,0,NULL,NULL); /* re-init */
	  HMAC_Update(&ctx,A1,A1_len);
	  HMAC_Update(&ctx_tmp,A1,A1_len);
	  HMAC_Update(&ctx,seed,seed_len);

	  if (olen > chunk)
	    {
	      HMAC_Final(&ctx,out,&j);
	      out+=j;
	      olen-=j;
	      HMAC_Final(&ctx_tmp,A1,&A1_len); /* calc the next A1 value */
	    }
	  else	/* last one */
	    {
	      HMAC_Final(&ctx,A1,&A1_len);
	      memcpy(out,A1,olen);
	      break;
	    }
	}
      HMAC_CTX_cleanup(&ctx);
      HMAC_CTX_cleanup(&ctx_tmp);
      std::memset(A1, 0, sizeof(A1));
    }

    static void PRF (unsigned char *label,
		     const size_t label_len,
		     const unsigned char *sec,
		     const size_t slen,
		     unsigned char *out1,
		     const size_t olen)
    {
      const EVP_MD *md5 = EVP_md5();
      const EVP_MD *sha1 = EVP_sha1();
      size_t len, i;
      const unsigned char *S1, *S2;
      unsigned char *out2;

      out2 = new unsigned char[olen];

      len = slen / 2;
      S1 = sec;
      S2 = &(sec[len]);
      len += (slen & 1); /* add for odd, make longer */
	
      hash(md5 ,S1,len,label,label_len,out1,olen);
      hash(sha1,S2,len,label,label_len,out2,olen);

      for (i=0; i<olen; i++)
	out1[i]^=out2[i];

      std::memset(out2, 0, olen);
      delete [] out2;
    }

    static void gen_exp(OpenVPNStaticKey& dest, const TLSPRF& client, const ProtoSessionID& psid_client,
			const TLSPRF& server, const ProtoSessionID& psid_server)
    {
      static const char master_secret_id[] = "OpenVPN master secret";
      static const char key_expansion_id[] = "OpenVPN key expansion";

      unsigned char master[48];

      client.verify_initialized();
      server.verify_initialized();

      // compute master secret
      openvpn_PRF (client.pre_master,
		   sizeof(client.pre_master),
		   master_secret_id,
		   client.random1,
		   sizeof(client.random1),
		   server.random1,
		   sizeof(server.random1),
		   NULL,
		   NULL,
		   master,
		   sizeof(master));
  
      // compute key expansion */
      openvpn_PRF (master,
		   sizeof(master),
		   key_expansion_id,
		   client.random2,
		   sizeof(client.random2),
		   server.random2,
		   sizeof(server.random2),
		   &psid_client,
		   &psid_server,
		   dest.raw_alloc(),
		   OpenVPNStaticKey::KEY_SIZE);

      std::memset(master, 0, sizeof(master));
    }

    void verify_initialized() const
    {
      if (!initialized_)
	throw tlsprf_uninitialized();
    }

    bool initialized_;
    bool server_;
    unsigned char pre_master[48]; // client generated
    unsigned char random1[32];    // generated by both client and server
    unsigned char random2[32];    // generated by both client and server
  };

} // namespace openvpn

#endif // OPENVPN_SSL_TLSPRF_H
