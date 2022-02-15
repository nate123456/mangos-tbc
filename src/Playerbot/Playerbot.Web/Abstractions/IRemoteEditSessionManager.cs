using Playerbot.Web.Models;

namespace Playerbot.Web.Abstractions;

public interface IRemoteEditSessionManager
{
    Task<IEnumerable<RemoteEditSession>> GetSessionsAsync();
    Task<RemoteEditSession> StartSessionAsync(int accountId);
    Task StopSessionAsync(RemoteEditSession session);
}
