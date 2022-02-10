using PlayerbotWeb.Models;

namespace PlayerbotWeb.Services;

public class AppState
{
    public List<PlayerbotScript>? CurrentScripts { get; set; }
    public List<PlayerbotScript>? NewScripts { get; set; }
    public PlayerbotToken? Token { get; set; }
    public string? SourceFolder { get; set; }
}
