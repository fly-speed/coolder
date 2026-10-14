#include "bridge_internal.h"
#include <cstdlib>
#include <cstring>
char *bridge_owned(const std::string &text)
{
	char *copy = static_cast<char *>(std::malloc(text.size() + 1));
	if (!copy)
		throw std::bad_alloc();
	std::memcpy(copy, text.c_str(), text.size() + 1);
	return copy;
}
extern "C" {
libai_cancellation *libai_invocation_retain_cancellation(const libai_invocation *invocation)
{
    try {
        if(!invocation)throw std::invalid_argument("invocation is null");
        if(!invocation->cancellation)return nullptr;
        return new libai_cancellation{invocation->cancellation};
    } catch(const std::exception &e){bridge_error(e.what());}
    catch(...){bridge_error("invocation cancellation retain failed");}
    return nullptr;
}
int64_t libai_invocation_remaining_ms(const libai_invocation *invocation)
{
    if(!invocation)return 0;
    if(invocation->deadline==std::chrono::steady_clock::time_point::max())return -1;
    auto remaining=invocation->deadline-std::chrono::steady_clock::now();
    if(remaining<=std::chrono::steady_clock::duration::zero())return 0;
    return std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
}
libai_cancellation *libai_cancellation_create(void)
{
	try {
		bridge_error("");
		return new libai_cancellation{
			std::make_shared<libai::cancellation_t>()
		};
	} catch (...) {
		bridge_error("cancellation allocation failed");
		return nullptr;
	}
}
libai_cancellation *libai_cancellation_retain(const libai_cancellation *token)
{
	try {
		bridge_error("");
		if (!token)
			throw std::invalid_argument("cancellation is null");
		return new libai_cancellation{ token->value };
	} catch (const std::exception &error) {
		bridge_error(error.what());
	} catch (...) {
		bridge_error("cancellation retain failed");
	}
	return nullptr;
}
void libai_cancellation_destroy(libai_cancellation *token)
{
	delete token;
}
void libai_cancellation_cancel(libai_cancellation *token)
{
	if (token)
		token->value->cancel();
}
int libai_cancellation_cancelled(const libai_cancellation *token)
{
	return token && token->value->cancelled();
}
}
